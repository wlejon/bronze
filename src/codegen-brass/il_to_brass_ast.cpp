#include "codegen-brass/il_to_brass_ast.h"
#include "abi/bronze_abi.h"
#include "support/source.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::codegen {

namespace {

constexpr il2mir::BronzeType mapType(il::Type t) noexcept {
    switch (t) {
        case il::Type::Void: return il2mir::BronzeType::Void;
        case il::Type::Bool: return il2mir::BronzeType::Bool;
        case il::Type::I32: return il2mir::BronzeType::I32;
        case il::Type::F64: return il2mir::BronzeType::F64;
        case il::Type::Str: return il2mir::BronzeType::Str;
        case il::Type::Dynamic: return il2mir::BronzeType::Dynamic;
    }
    return il2mir::BronzeType::Unknown;
}

il2mir::BronzeBlockTarget mapBlockTarget(const il::BlockTarget& target) {
    il2mir::BronzeBlockTarget bt;
    bt.block_id = target.block;
    bt.args = target.args;
    return bt;
}

struct LowerContext {
    const il::Module& module;
    const std::vector<std::string>& uniqueNames;
    const std::vector<LineTable>& lineTables;

    const std::string& fnName(size_t idx) const noexcept {
        if (idx < uniqueNames.size() && !uniqueNames[idx].empty()) {
            return uniqueNames[idx];
        }
        if (idx < module.functions.size()) {
            return module.functions[idx].name;
        }
        static const std::string kUnknownFn = "?";
        return kUnknownFn;
    }

    const std::string& keyString(uint32_t keyIndex) const noexcept {
        if (keyIndex < module.keyConstants.size()) {
            return module.keyConstants[keyIndex];
        }
        static const std::string kEmptyString;
        return kEmptyString;
    }
};

il2mir::BronzeInstruction lowerInstruction(const il::Instruction& inst, const LowerContext& ctx) {
    il2mir::BronzeInstruction out;
    out.result_id = (inst.result != il::kNoValue) ? inst.result : UINT32_MAX;
    out.result_type = mapType(inst.type);

    if (inst.op != il::Op::GlobalGet) {
        out.operands = inst.operands;
    }
    if (inst.op == il::Op::Jump) {
        out.target = mapBlockTarget(inst.target);
    } else if (inst.op == il::Op::Branch) {
        out.target = mapBlockTarget(inst.target);
        out.else_target = mapBlockTarget(inst.elseTarget);
    }

    // The (line, column) brass records as the instruction's debug location,
    // which is what an Error.stack frame reports (bronze_pc_entry).
    if ((inst.span.begin != 0 || inst.span.end != 0) && inst.span.file < ctx.lineTables.size()) {
        const SourceBuffer::LineCol lc = ctx.lineTables[inst.span.file].lineCol(inst.span.begin);
        out.line = lc.line;
        out.column = lc.column;
    }

    switch (inst.op) {
        // --- Constants ---
        case il::Op::ConstF64:
            out.op = il2mir::BronzeOp::ConstF64;
            out.imm_f64 = inst.immF64;
            break;
        case il::Op::ConstI32:
            out.op = il2mir::BronzeOp::ConstI32;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ConstBool:
            out.op = il2mir::BronzeOp::ConstBool;
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::ConstUndefined:
            out.op = il2mir::BronzeOp::ConstUndefined;
            break;
        case il::Op::ConstNull:
            out.op = il2mir::BronzeOp::ConstNull;
            break;
        case il::Op::ConstBigInt:
            out.op = il2mir::BronzeOp::ConstBigInt;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;

        // --- Arithmetic & String Operations ---
        case il::Op::Add: out.op = il2mir::BronzeOp::Add; break;
        case il::Op::ConcatBegin:
            out.op = il2mir::BronzeOp::ConcatBegin;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ConcatAppend: out.op = il2mir::BronzeOp::ConcatAppend; break;
        case il::Op::ConcatEnd: out.op = il2mir::BronzeOp::ConcatEnd; break;
        case il::Op::Sub: out.op = il2mir::BronzeOp::Sub; break;
        case il::Op::Neg: out.op = il2mir::BronzeOp::Neg; break;
        case il::Op::Mul: out.op = il2mir::BronzeOp::Mul; break;
        case il::Op::Div: out.op = il2mir::BronzeOp::Div; break;
        case il::Op::Mod: out.op = il2mir::BronzeOp::Mod; break;
        case il::Op::Pow: out.op = il2mir::BronzeOp::Pow; break;
        case il::Op::ToNumeric: out.op = il2mir::BronzeOp::ToNumeric; break;
        case il::Op::NumericStep:
            out.op = il2mir::BronzeOp::NumericStep;
            out.imm_i64 = (inst.immI32 > 0) ? 1 : -1;
            break;
        case il::Op::ToInt32: out.op = il2mir::BronzeOp::ToInt32; break;

        // --- Bitwise Operations ---
        case il::Op::BitAnd: out.op = il2mir::BronzeOp::BitAnd; break;
        case il::Op::BitOr: out.op = il2mir::BronzeOp::BitOr; break;
        case il::Op::BitXor: out.op = il2mir::BronzeOp::BitXor; break;
        case il::Op::MathImul: out.op = il2mir::BronzeOp::MathImul; break;
        case il::Op::Shl: out.op = il2mir::BronzeOp::Shl; break;
        case il::Op::Shr: out.op = il2mir::BronzeOp::Shr; break;
        case il::Op::UShr: out.op = il2mir::BronzeOp::UShr; break;
        case il::Op::BitNot: out.op = il2mir::BronzeOp::BitNot; break;

        // --- Comparisons & Predicates ---
        case il::Op::CmpLt: out.op = il2mir::BronzeOp::CmpLt; break;
        case il::Op::CmpGt: out.op = il2mir::BronzeOp::CmpGt; break;
        case il::Op::CmpLe: out.op = il2mir::BronzeOp::CmpLe; break;
        case il::Op::CmpGe: out.op = il2mir::BronzeOp::CmpGe; break;
        case il::Op::CmpEq: out.op = il2mir::BronzeOp::CmpEq; break;
        case il::Op::CmpNe: out.op = il2mir::BronzeOp::CmpNe; break;
        case il::Op::NumTruthy: out.op = il2mir::BronzeOp::NumTruthy; break;
        case il::Op::StrictEq: out.op = il2mir::BronzeOp::StrictEq; break;
        case il::Op::LooseEq: out.op = il2mir::BronzeOp::LooseEq; break;
        case il::Op::RelLt: out.op = il2mir::BronzeOp::RelLt; break;
        case il::Op::RelGt: out.op = il2mir::BronzeOp::RelGt; break;
        case il::Op::RelLe: out.op = il2mir::BronzeOp::RelLe; break;
        case il::Op::RelGe: out.op = il2mir::BronzeOp::RelGe; break;
        case il::Op::TypeOf: out.op = il2mir::BronzeOp::TypeOf; break;
        case il::Op::ToStr: out.op = il2mir::BronzeOp::ToStr; break;
        case il::Op::InstanceOf: out.op = il2mir::BronzeOp::InstanceOf; break;
        case il::Op::In: out.op = il2mir::BronzeOp::In; break;
        case il::Op::IsNullish: out.op = il2mir::BronzeOp::IsNullish; break;
        case il::Op::IsNumber: out.op = il2mir::BronzeOp::IsNumber; break;
        case il::Op::IsDenseArray:
            out.op = il2mir::BronzeOp::IsDenseArray;
            out.imm_i64 = inst.immI32;
            break;

        // --- Control Flow & Call Termination ---
        case il::Op::Ret: out.op = il2mir::BronzeOp::Ret; break;
        case il::Op::Throw: out.op = il2mir::BronzeOp::Throw; break;
        case il::Op::ExcTake: out.op = il2mir::BronzeOp::ExcTake; break;
        case il::Op::Jump: out.op = il2mir::BronzeOp::Jump; break;
        case il::Op::Branch: out.op = il2mir::BronzeOp::Branch; break;
        case il::Op::Call:
            out.op = il2mir::BronzeOp::Call;
            out.callee_name = ctx.fnName(inst.calleeIndex);
            out.env_hops = (inst.callEnvHops != il::Instruction::kNoEnvHops) ? inst.callEnvHops : UINT32_MAX;
            break;

        // --- Boxing / Unboxing ---
        case il::Op::Box:
            out.op = il2mir::BronzeOp::Box;
            out.box_type = mapType(inst.boxType);
            out.index = inst.keyIndex;
            if (inst.operands.empty()) {
                out.string_literal = 'k';
                out.string_literal += std::to_string(inst.keyIndex);
            }
            break;
        case il::Op::Unbox:
            out.op = il2mir::BronzeOp::Unbox;
            out.raw_unbox = inst.rawUnbox;
            break;

        // --- Properties & Elements ---
        case il::Op::PropGet:
            out.op = il2mir::BronzeOp::PropGet;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            out.static_slot = inst.staticSlot;
            break;
        case il::Op::SuperGet:
            out.op = il2mir::BronzeOp::SuperGet;
            out.index = inst.keyIndex;
            break;
        case il::Op::SuperSet:
            out.op = il2mir::BronzeOp::SuperSet;
            out.index = inst.keyIndex;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::PropSet:
            out.op = il2mir::BronzeOp::PropSet;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            out.static_slot = inst.staticSlot;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemGet: out.op = il2mir::BronzeOp::ElemGet; break;
        case il::Op::ElemSet:
            out.op = il2mir::BronzeOp::ElemSet;
            out.index = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::ElemGetTyped:
            out.op = il2mir::BronzeOp::ElemGetTyped;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemSetTyped:
            out.op = il2mir::BronzeOp::ElemSetTyped;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::MathUnary:
            out.op = il2mir::BronzeOp::MathUnary;
            out.imm_i64 = inst.immI32;
            break;

        // --- Dynamic Calls & Objects ---
        case il::Op::DynamicCall:
            out.op = il2mir::BronzeOp::CallDynamic;
            out.param_count = inst.operands.size() >= 2 ? static_cast<uint32_t>(inst.operands.size() - 2) : 0;
            break;
        case il::Op::FunctionRef:
            out.op = il2mir::BronzeOp::FuncRef;
            out.callee_name = ctx.fnName(inst.calleeIndex);
            break;
        case il::Op::Construct:
            out.op = il2mir::BronzeOp::Construct;
            out.param_count = inst.operands.empty() ? 0 : static_cast<uint32_t>(inst.operands.size() - 1);
            break;
        case il::Op::CreateObject: out.op = il2mir::BronzeOp::CreateObject; break;
        case il::Op::CreateGeneratorObject: out.op = il2mir::BronzeOp::CreateGeneratorObject; break;
        case il::Op::CreateAsyncGeneratorObject: out.op = il2mir::BronzeOp::CreateAsyncGeneratorObject; break;
        case il::Op::CreateAsyncMachine: out.op = il2mir::BronzeOp::CreateAsyncMachine; break;
        case il::Op::AsyncStart: out.op = il2mir::BronzeOp::AsyncStart; break;
        case il::Op::AsyncAwait: out.op = il2mir::BronzeOp::AsyncAwait; break;
        case il::Op::DynamicImport:
            out.op = il2mir::BronzeOp::DynamicImport;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::ModuleNamespace: out.op = il2mir::BronzeOp::ModuleNamespace; break;
        case il::Op::ObjectKeys: out.op = il2mir::BronzeOp::ObjectKeys; break;
        case il::Op::ForInKeys: out.op = il2mir::BronzeOp::ForInKeys; break;
        case il::Op::MethodDef:
            out.op = il2mir::BronzeOp::MethodDef;
            out.index = inst.keyIndex;
            break;
        case il::Op::MethodDefComputed: out.op = il2mir::BronzeOp::MethodDefComputed; break;
        case il::Op::AccessorDef:
            out.op = il2mir::BronzeOp::AccessorDef;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::AccessorDefComputed:
            out.op = il2mir::BronzeOp::AccessorDefComputed;
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::DefineOwnAttr:
            out.op = il2mir::BronzeOp::DefineOwnAttr;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::GetNewTarget: out.op = il2mir::BronzeOp::GetNewTarget; break;
        case il::Op::ImportMeta:
            out.op = il2mir::BronzeOp::ImportMeta;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::SuperCall:
            out.op = il2mir::BronzeOp::SuperCall;
            out.param_count = inst.operands.size() >= 2 ? static_cast<uint32_t>(inst.operands.size() - 2) : 0;
            break;
        case il::Op::SuperCallSpread: out.op = il2mir::BronzeOp::SuperCallSpread; break;
        case il::Op::TemplateCached:
            out.op = il2mir::BronzeOp::TemplateCached;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::TemplateObject:
            out.op = il2mir::BronzeOp::TemplateObject;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ArrayAppendHole: out.op = il2mir::BronzeOp::ArrayAppendHole; break;
        case il::Op::PropDelete:
            out.op = il2mir::BronzeOp::PropDelete;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemDelete:
            out.op = il2mir::BronzeOp::ElemDelete;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::GlobalGet:
            // A cached read: calls the per-key thunk that queries the module's cache slot.
            out.op = il2mir::BronzeOp::Call;
            out.callee_name = globalReadThunkName(inst.keyIndex);
            break;
        case il::Op::ResolveName:
            out.op = il2mir::BronzeOp::NameResolve;
            out.string_literal = ctx.keyString(inst.keyIndex);
            out.index = (inst.immI32 != 0) ? 1 : 0;
            break;
        case il::Op::ImmutableAssign:
            out.op = il2mir::BronzeOp::ImmutableAssign;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::PinGuard:
            out.op = il2mir::BronzeOp::PinGuard;
            out.imm_i64 = static_cast<int64_t>(inst.immI32);
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::CensusRecord:
            out.op = il2mir::BronzeOp::CensusRecord;
            out.imm_i64 = inst.immI32 & BRONZE_ABI_CENSUS_KIND_MASK;
            out.index = inst.keyIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::ClassExtend: out.op = il2mir::BronzeOp::ClassExtend; break;
        case il::Op::PrivateNew: out.op = il2mir::BronzeOp::PrivateNew; break;
        case il::Op::PrivateHas:
            out.op = il2mir::BronzeOp::PrivateHas;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::PrivateGet:
            out.op = il2mir::BronzeOp::PrivateGet;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::PrivateAdd: out.op = il2mir::BronzeOp::PrivateAdd; break;
        case il::Op::PrivateSet:
            out.op = il2mir::BronzeOp::PrivateSet;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::PrivateMisuse:
            out.op = il2mir::BronzeOp::PrivateMisuse;
            out.string_literal = ctx.keyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterOpen: out.op = il2mir::BronzeOp::IterOpen; break;
        case il::Op::AsyncIterOpen: out.op = il2mir::BronzeOp::AsyncIterOpen; break;
        case il::Op::AsyncIterNext: out.op = il2mir::BronzeOp::AsyncIterNext; break;
        case il::Op::AsyncIterClose:
            out.op = il2mir::BronzeOp::AsyncIterClose;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterStep: out.op = il2mir::BronzeOp::IterStep; break;
        case il::Op::IterValue: out.op = il2mir::BronzeOp::IterValue; break;
        case il::Op::IterClose:
            out.op = il2mir::BronzeOp::IterClose;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterRest: out.op = il2mir::BronzeOp::IterRest; break;
        case il::Op::IterDelegate: out.op = il2mir::BronzeOp::IterDelegate; break;
        case il::Op::PatternCheck:
            out.op = il2mir::BronzeOp::PatternCheck;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ArrayAppend: out.op = il2mir::BronzeOp::ArrayAppend; break;
        case il::Op::ArraySpread: out.op = il2mir::BronzeOp::ArraySpread; break;
        case il::Op::ObjectSpread: out.op = il2mir::BronzeOp::ObjectSpread; break;
        case il::Op::ObjectRest: out.op = il2mir::BronzeOp::ObjectRest; break;
        case il::Op::DynamicCallSpread: out.op = il2mir::BronzeOp::DynamicCallSpread; break;
        case il::Op::MethodCall:
            out.op = il2mir::BronzeOp::MethodCall;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            if (inst.directTarget != il::Instruction::kNoDirectTarget) {
                out.callee_name = ctx.fnName(inst.directTarget);
            }
            out.param_count = inst.operands.size() > 1 ? static_cast<uint32_t>(inst.operands.size() - 1) : 0;
            break;
        case il::Op::MethodCallSpread:
            out.op = il2mir::BronzeOp::MethodCallSpread;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            break;
        case il::Op::ConstructSpread: out.op = il2mir::BronzeOp::ConstructSpread; break;
        case il::Op::CreateArray:
            out.op = il2mir::BronzeOp::CreateArray;
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::CreateFunction:
            out.op = il2mir::BronzeOp::CreateFunc;
            out.callee_name = ctx.fnName(inst.calleeIndex);
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::EnvCreate:
            out.op = il2mir::BronzeOp::EnvCreate;
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::EnvGet:
            out.op = il2mir::BronzeOp::EnvGet;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvSet:
            out.op = il2mir::BronzeOp::EnvSet;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvInitTdz:
            out.op = il2mir::BronzeOp::EnvInitTdz;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvGetTdz:
            out.op = il2mir::BronzeOp::EnvGetTdz;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            out.string_literal = ctx.keyString(inst.keyIndex);
            break;
        case il::Op::ModuleEnvSet: out.op = il2mir::BronzeOp::ModuleEnvSet; break;
        case il::Op::ModuleEnvGet: out.op = il2mir::BronzeOp::ModuleEnvGet; break;
        case il::Op::Print: out.op = il2mir::BronzeOp::Print; break;
        case il::Op::PrintErr: out.op = il2mir::BronzeOp::PrintErr; break;
        case il::Op::PrintSpread: out.op = il2mir::BronzeOp::PrintSpread; break;
        case il::Op::PrintSpreadErr: out.op = il2mir::BronzeOp::PrintSpreadErr; break;
    }

    return out;
}

} // namespace

std::string globalReadThunkName(uint32_t keyIndex) {
    return "__bronze_global_read_k" + std::to_string(keyIndex);
}

il2mir::BronzeModuleAST lowerToBrassAst(
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    std::vector<uint32_t>* globalReadKeys
) {
    il2mir::BronzeModuleAST ast;
    ast.name = !module.name.empty() ? module.name : (!module.sourceFiles.empty() ? module.sourceFiles[0] : "");
    ast.functions.reserve(module.functions.size());

    if (globalReadKeys) {
        globalReadKeys->clear();
    }

    // The module's own line index and not one built from `sourceTexts`,
    // which `--no-fn-source` has emptied by now.
    const std::vector<LineTable>& lineTables = module.lineTables;
    const LowerContext ctx{module, uniqueNames, lineTables};

    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        const auto& fn = module.functions[fnIdx];

        il2mir::BronzeFunction bfn;
        bfn.name = (fnIdx < uniqueNames.size() && !uniqueNames[fnIdx].empty())
                       ? uniqueNames[fnIdx]
                       : fn.name;
        bfn.return_type = (fn.returnType == il::Type::Bool)
                              ? il2mir::BronzeType::I32
                              : mapType(fn.returnType);
        bfn.is_exported = fn.isExported;

        bfn.params.reserve(fn.params.size());
        for (size_t p = 0; p < fn.params.size(); ++p) {
            bfn.params.push_back({static_cast<uint32_t>(p), mapType(fn.params[p].type)});
        }

        if (fn.blocks.empty()) {
            ast.functions.push_back(std::move(bfn));
            continue;
        }

        bfn.blocks.reserve(fn.blocks.size());
        for (const auto& block : fn.blocks) {
            il2mir::BronzeBlock bblk;
            bblk.id = block.id;
            bblk.handler_id = (block.handler != il::kNoBlock) ? block.handler : UINT32_MAX;

            bblk.params.reserve(block.params.size());
            for (const auto& p : block.params) {
                bblk.params.push_back({p.id, mapType(p.type)});
            }

            bblk.instructions.reserve(block.instructions.size());
            for (const auto& inst : block.instructions) {
                if (globalReadKeys && inst.op == il::Op::GlobalGet) {
                    globalReadKeys->push_back(inst.keyIndex);
                }
                bblk.instructions.push_back(lowerInstruction(inst, ctx));
            }

            bfn.blocks.push_back(std::move(bblk));
        }

        ast.functions.push_back(std::move(bfn));
    }

    if (globalReadKeys && !globalReadKeys->empty()) {
        std::sort(globalReadKeys->begin(), globalReadKeys->end());
        globalReadKeys->erase(std::unique(globalReadKeys->begin(), globalReadKeys->end()),
                              globalReadKeys->end());
    }

    return ast;
}

} // namespace bronze::codegen
