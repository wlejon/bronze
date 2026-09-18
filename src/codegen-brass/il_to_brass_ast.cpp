#include "codegen-brass/il_to_brass_ast.h"
#include "abi/bronze_abi.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace bronze::codegen {

static brass::il::BronzeType mapType(il::Type t) {
    switch (t) {
        case il::Type::Void: return brass::il::BronzeType::Void;
        case il::Type::Bool: return brass::il::BronzeType::Bool;
        case il::Type::I32: return brass::il::BronzeType::I32;
        case il::Type::F64: return brass::il::BronzeType::F64;
        case il::Type::Str: return brass::il::BronzeType::Str;
        case il::Type::Dynamic: return brass::il::BronzeType::Dynamic;
    }
    return brass::il::BronzeType::Unknown;
}

static brass::il::BronzeBlockTarget mapBlockTarget(const il::BlockTarget& target) {
    brass::il::BronzeBlockTarget bt;
    bt.block_id = target.block;
    bt.args = target.args;
    return bt;
}

static brass::il::BronzeInstruction lowerInstruction(
    const il::Instruction& inst,
    const il::Module& module,
    const std::vector<std::string>& uniqueNames
) {
    brass::il::BronzeInstruction out;
    out.result_id = (inst.result != il::kNoValue) ? inst.result : UINT32_MAX;
    out.result_type = mapType(inst.type);
    out.operands = inst.operands;
    out.target = mapBlockTarget(inst.target);
    out.else_target = mapBlockTarget(inst.elseTarget);

    auto getFnName = [&](size_t idx) -> std::string {
        if (idx < uniqueNames.size() && !uniqueNames[idx].empty()) {
            return uniqueNames[idx];
        }
        return (idx < module.functions.size()) ? module.functions[idx].name : "?";
    };

    auto getKeyString = [&](uint32_t keyIndex) -> std::string {
        if (keyIndex < module.keyConstants.size()) {
            return module.keyConstants[keyIndex];
        }
        return std::string();
    };

    switch (inst.op) {
        case il::Op::ConstF64:
            out.op = brass::il::BronzeOp::ConstF64;
            out.imm_f64 = inst.immF64;
            break;
        case il::Op::ConstI32:
            out.op = brass::il::BronzeOp::ConstI32;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ConstBool:
            out.op = brass::il::BronzeOp::ConstBool;
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::ConstUndefined:
            out.op = brass::il::BronzeOp::ConstUndefined;
            break;
        case il::Op::ConstNull:
            out.op = brass::il::BronzeOp::ConstNull;
            break;
        case il::Op::ConstBigInt:
            out.op = brass::il::BronzeOp::ConstBigInt;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::Add:
            out.op = brass::il::BronzeOp::Add;
            break;
        case il::Op::ConcatBegin:
            out.op = brass::il::BronzeOp::ConcatBegin;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ConcatAppend:
            out.op = brass::il::BronzeOp::ConcatAppend;
            break;
        case il::Op::ConcatEnd:
            out.op = brass::il::BronzeOp::ConcatEnd;
            break;
        case il::Op::Sub:
            out.op = brass::il::BronzeOp::Sub;
            break;
        case il::Op::Neg:
            out.op = brass::il::BronzeOp::Neg;
            break;
        case il::Op::Mul:
            out.op = brass::il::BronzeOp::Mul;
            break;
        case il::Op::Div:
            out.op = brass::il::BronzeOp::Div;
            break;
        case il::Op::Mod:
            out.op = brass::il::BronzeOp::Mod;
            break;
        case il::Op::Pow:
            out.op = brass::il::BronzeOp::Pow;
            break;
        case il::Op::ToNumeric:
            out.op = brass::il::BronzeOp::ToNumeric;
            break;
        case il::Op::NumericStep:
            out.op = brass::il::BronzeOp::NumericStep;
            out.imm_i64 = (inst.immI32 > 0) ? 1 : -1;
            break;
        case il::Op::ToInt32:
            out.op = brass::il::BronzeOp::ToInt32;
            break;
        case il::Op::BitAnd:
            out.op = brass::il::BronzeOp::BitAnd;
            break;
        case il::Op::BitOr:
            out.op = brass::il::BronzeOp::BitOr;
            break;
        case il::Op::BitXor:
            out.op = brass::il::BronzeOp::BitXor;
            break;
        case il::Op::MathImul:
            out.op = brass::il::BronzeOp::MathImul;
            break;
        case il::Op::Shl:
            out.op = brass::il::BronzeOp::Shl;
            break;
        case il::Op::Shr:
            out.op = brass::il::BronzeOp::Shr;
            break;
        case il::Op::UShr:
            out.op = brass::il::BronzeOp::UShr;
            break;
        case il::Op::BitNot:
            out.op = brass::il::BronzeOp::BitNot;
            break;
        case il::Op::CmpLt:
            out.op = brass::il::BronzeOp::CmpLt;
            break;
        case il::Op::CmpGt:
            out.op = brass::il::BronzeOp::CmpGt;
            break;
        case il::Op::CmpLe:
            out.op = brass::il::BronzeOp::CmpLe;
            break;
        case il::Op::CmpGe:
            out.op = brass::il::BronzeOp::CmpGe;
            break;
        case il::Op::CmpEq:
            out.op = brass::il::BronzeOp::CmpEq;
            break;
        case il::Op::CmpNe:
            out.op = brass::il::BronzeOp::CmpNe;
            break;
        case il::Op::NumTruthy:
            out.op = brass::il::BronzeOp::NumTruthy;
            break;
        case il::Op::StrictEq:
            out.op = brass::il::BronzeOp::StrictEq;
            break;
        case il::Op::LooseEq:
            out.op = brass::il::BronzeOp::LooseEq;
            break;
        case il::Op::RelLt:
            out.op = brass::il::BronzeOp::RelLt;
            break;
        case il::Op::RelGt:
            out.op = brass::il::BronzeOp::RelGt;
            break;
        case il::Op::RelLe:
            out.op = brass::il::BronzeOp::RelLe;
            break;
        case il::Op::RelGe:
            out.op = brass::il::BronzeOp::RelGe;
            break;
        case il::Op::TypeOf:
            out.op = brass::il::BronzeOp::TypeOf;
            break;
        case il::Op::ToStr:
            out.op = brass::il::BronzeOp::ToStr;
            break;
        case il::Op::InstanceOf:
            out.op = brass::il::BronzeOp::InstanceOf;
            break;
        case il::Op::In:
            out.op = brass::il::BronzeOp::In;
            break;
        case il::Op::IsNullish:
            out.op = brass::il::BronzeOp::IsNullish;
            break;
        case il::Op::IsNumber:
            out.op = brass::il::BronzeOp::IsNumber;
            break;
        case il::Op::IsDenseArray:
            out.op = brass::il::BronzeOp::IsDenseArray;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::Ret:
            out.op = brass::il::BronzeOp::Ret;
            break;
        case il::Op::Throw:
            out.op = brass::il::BronzeOp::Throw;
            break;
        case il::Op::ExcTake:
            out.op = brass::il::BronzeOp::ExcTake;
            break;
        case il::Op::Jump:
            out.op = brass::il::BronzeOp::Jump;
            break;
        case il::Op::Branch:
            out.op = brass::il::BronzeOp::Branch;
            break;
        case il::Op::Call:
            out.op = brass::il::BronzeOp::Call;
            out.callee_name = getFnName(inst.calleeIndex);
            out.env_hops = (inst.callEnvHops != il::Instruction::kNoEnvHops) ? inst.callEnvHops : UINT32_MAX;
            break;
        case il::Op::Box:
            out.op = brass::il::BronzeOp::Box;
            out.box_type = mapType(inst.boxType);
            out.index = inst.keyIndex;
            if (inst.operands.empty()) {
                out.string_literal = 'k';
                out.string_literal += std::to_string(inst.keyIndex);
            }
            break;
        case il::Op::Unbox:
            out.op = brass::il::BronzeOp::Unbox;
            out.raw_unbox = inst.rawUnbox;
            break;
        case il::Op::PropGet:
            out.op = brass::il::BronzeOp::PropGet;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            out.static_slot = inst.staticSlot;
            break;
        case il::Op::SuperGet:
            out.op = brass::il::BronzeOp::SuperGet;
            out.index = inst.keyIndex;
            break;
        case il::Op::SuperSet:
            out.op = brass::il::BronzeOp::SuperSet;
            out.index = inst.keyIndex;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::PropSet:
            out.op = brass::il::BronzeOp::PropSet;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            out.static_slot = inst.staticSlot;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemGet:
            out.op = brass::il::BronzeOp::ElemGet;
            break;
        case il::Op::ElemSet:
            out.op = brass::il::BronzeOp::ElemSet;
            out.index = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::ElemGetTyped:
            out.op = brass::il::BronzeOp::ElemGetTyped;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemSetTyped:
            out.op = brass::il::BronzeOp::ElemSetTyped;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::MathUnary:
            out.op = brass::il::BronzeOp::MathUnary;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::DynamicCall:
            out.op = brass::il::BronzeOp::CallDynamic;
            out.param_count = inst.operands.size() >= 2 ? static_cast<uint32_t>(inst.operands.size() - 2) : 0;
            break;
        case il::Op::FunctionRef:
            out.op = brass::il::BronzeOp::FuncRef;
            out.callee_name = getFnName(inst.calleeIndex);
            break;
        case il::Op::Construct:
            out.op = brass::il::BronzeOp::Construct;
            out.param_count = inst.operands.empty() ? 0 : static_cast<uint32_t>(inst.operands.size() - 1);
            break;
        case il::Op::CreateObject:
            out.op = brass::il::BronzeOp::CreateObject;
            break;
        case il::Op::CreateGeneratorObject:
            out.op = brass::il::BronzeOp::CreateGeneratorObject;
            break;
        case il::Op::CreateAsyncGeneratorObject:
            out.op = brass::il::BronzeOp::CreateAsyncGeneratorObject;
            break;
        case il::Op::CreateAsyncMachine:
            out.op = brass::il::BronzeOp::CreateAsyncMachine;
            break;
        case il::Op::AsyncStart:
            out.op = brass::il::BronzeOp::AsyncStart;
            break;
        case il::Op::AsyncAwait:
            out.op = brass::il::BronzeOp::AsyncAwait;
            break;
        case il::Op::DynamicImport:
            out.op = brass::il::BronzeOp::DynamicImport;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::ModuleNamespace:
            out.op = brass::il::BronzeOp::ModuleNamespace;
            break;
        case il::Op::ObjectKeys:
            out.op = brass::il::BronzeOp::ObjectKeys;
            break;
        case il::Op::ForInKeys:
            out.op = brass::il::BronzeOp::ForInKeys;
            break;
        case il::Op::MethodDef:
            out.op = brass::il::BronzeOp::MethodDef;
            out.index = inst.keyIndex;
            break;
        case il::Op::MethodDefComputed:
            out.op = brass::il::BronzeOp::MethodDefComputed;
            break;
        case il::Op::AccessorDef:
            out.op = brass::il::BronzeOp::AccessorDef;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::AccessorDefComputed:
            out.op = brass::il::BronzeOp::AccessorDefComputed;
            out.imm_bool = (inst.immI32 != 0);
            break;
        case il::Op::DefineOwnAttr:
            out.op = brass::il::BronzeOp::DefineOwnAttr;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::GetNewTarget:
            out.op = brass::il::BronzeOp::GetNewTarget;
            break;
        case il::Op::ImportMeta:
            out.op = brass::il::BronzeOp::ImportMeta;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::SuperCall:
            out.op = brass::il::BronzeOp::SuperCall;
            out.param_count = inst.operands.size() >= 2 ? static_cast<uint32_t>(inst.operands.size() - 2) : 0;
            break;
        case il::Op::SuperCallSpread:
            out.op = brass::il::BronzeOp::SuperCallSpread;
            break;
        case il::Op::TemplateCached:
            out.op = brass::il::BronzeOp::TemplateCached;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::TemplateObject:
            out.op = brass::il::BronzeOp::TemplateObject;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ArrayAppendHole:
            out.op = brass::il::BronzeOp::ArrayAppendHole;
            break;
        case il::Op::PropDelete:
            out.op = brass::il::BronzeOp::PropDelete;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ElemDelete:
            out.op = brass::il::BronzeOp::ElemDelete;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::GlobalGet:
            // A cached read: the thunk loads the module's cache slot for this
            // key and only calls into the runtime while the slot is empty.
            out.op = brass::il::BronzeOp::Call;
            out.callee_name = globalReadThunkName(inst.keyIndex);
            out.operands.clear();
            break;
        case il::Op::ResolveName:
            out.op = brass::il::BronzeOp::NameResolve;
            out.string_literal = getKeyString(inst.keyIndex);
            out.index = (inst.immI32 != 0) ? 1 : 0;
            break;
        case il::Op::ImmutableAssign:
            out.op = brass::il::BronzeOp::ImmutableAssign;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::PinGuard:
            out.op = brass::il::BronzeOp::PinGuard;
            out.imm_i64 = static_cast<int64_t>(inst.immI32);
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::CensusRecord:
            out.op = brass::il::BronzeOp::CensusRecord;
            out.imm_i64 = inst.immI32 & BRONZE_ABI_CENSUS_KIND_MASK;
            out.index = inst.keyIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::ClassExtend:
            out.op = brass::il::BronzeOp::ClassExtend;
            break;
        case il::Op::PrivateNew:
            out.op = brass::il::BronzeOp::PrivateNew;
            break;
        case il::Op::PrivateHas:
            out.op = brass::il::BronzeOp::PrivateHas;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::PrivateGet:
            out.op = brass::il::BronzeOp::PrivateGet;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::PrivateAdd:
            out.op = brass::il::BronzeOp::PrivateAdd;
            break;
        case il::Op::PrivateSet:
            out.op = brass::il::BronzeOp::PrivateSet;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::PrivateMisuse:
            out.op = brass::il::BronzeOp::PrivateMisuse;
            out.string_literal = getKeyString(inst.keyIndex);
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterOpen:
            out.op = brass::il::BronzeOp::IterOpen;
            break;
        case il::Op::AsyncIterOpen:
            out.op = brass::il::BronzeOp::AsyncIterOpen;
            break;
        case il::Op::AsyncIterNext:
            out.op = brass::il::BronzeOp::AsyncIterNext;
            break;
        case il::Op::AsyncIterClose:
            out.op = brass::il::BronzeOp::AsyncIterClose;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterStep:
            out.op = brass::il::BronzeOp::IterStep;
            break;
        case il::Op::IterValue:
            out.op = brass::il::BronzeOp::IterValue;
            break;
        case il::Op::IterClose:
            out.op = brass::il::BronzeOp::IterClose;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::IterRest:
            out.op = brass::il::BronzeOp::IterRest;
            break;
        case il::Op::IterDelegate:
            out.op = brass::il::BronzeOp::IterDelegate;
            break;
        case il::Op::PatternCheck:
            out.op = brass::il::BronzeOp::PatternCheck;
            out.imm_i64 = inst.immI32;
            break;
        case il::Op::ArrayAppend:
            out.op = brass::il::BronzeOp::ArrayAppend;
            break;
        case il::Op::ArraySpread:
            out.op = brass::il::BronzeOp::ArraySpread;
            break;
        case il::Op::ObjectSpread:
            out.op = brass::il::BronzeOp::ObjectSpread;
            break;
        case il::Op::ObjectRest:
            out.op = brass::il::BronzeOp::ObjectRest;
            break;
        case il::Op::DynamicCallSpread:
            out.op = brass::il::BronzeOp::DynamicCallSpread;
            break;
        case il::Op::MethodCall:
            out.op = brass::il::BronzeOp::MethodCall;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            out.is_fn_recv = inst.icFnRecv;
            if (inst.directTarget != il::Instruction::kNoDirectTarget) {
                out.callee_name = getFnName(inst.directTarget);
            }
            out.param_count = inst.operands.size() > 1 ? static_cast<uint32_t>(inst.operands.size() - 1) : 0;
            break;
        case il::Op::MethodCallSpread:
            out.op = brass::il::BronzeOp::MethodCallSpread;
            out.index = inst.keyIndex;
            out.depth = inst.icIndex;
            out.ic_index = inst.icIndex;
            out.is_mono = inst.icMonomorphic;
            break;
        case il::Op::ConstructSpread:
            out.op = brass::il::BronzeOp::ConstructSpread;
            break;
        case il::Op::CreateArray:
            out.op = brass::il::BronzeOp::CreateArray;
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::CreateFunction:
            out.op = brass::il::BronzeOp::CreateFunc;
            out.callee_name = getFnName(inst.calleeIndex);
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::EnvCreate:
            out.op = brass::il::BronzeOp::EnvCreate;
            out.param_count = static_cast<uint32_t>(inst.immI32);
            break;
        case il::Op::EnvGet:
            out.op = brass::il::BronzeOp::EnvGet;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvSet:
            out.op = brass::il::BronzeOp::EnvSet;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvInitTdz:
            out.op = brass::il::BronzeOp::EnvInitTdz;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            break;
        case il::Op::EnvGetTdz:
            out.op = brass::il::BronzeOp::EnvGetTdz;
            out.depth = inst.envDepth;
            out.index = inst.envIndex;
            out.string_literal = getKeyString(inst.keyIndex);
            break;
        case il::Op::ModuleEnvSet:
            out.op = brass::il::BronzeOp::ModuleEnvSet;
            break;
        case il::Op::ModuleEnvGet:
            out.op = brass::il::BronzeOp::ModuleEnvGet;
            break;
        case il::Op::Print:
            out.op = brass::il::BronzeOp::Print;
            break;
        case il::Op::PrintErr:
            out.op = brass::il::BronzeOp::PrintErr;
            break;
        case il::Op::PrintSpread:
            out.op = brass::il::BronzeOp::PrintSpread;
            break;
        case il::Op::PrintSpreadErr:
            out.op = brass::il::BronzeOp::PrintSpreadErr;
            break;
    }

    return out;
}

std::string globalReadThunkName(uint32_t keyIndex) {
    return "__bronze_global_read_k" + std::to_string(keyIndex);
}

brass::il::BronzeModuleAST lowerToBrassAst(
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    std::vector<uint32_t>* globalReadKeys
) {
    brass::il::BronzeModuleAST ast;
    ast.name = module.name;

    std::vector<uint32_t> readKeys;
    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        const auto& fn = module.functions[fnIdx];
        if (fn.blocks.empty()) continue;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::GlobalGet) readKeys.push_back(inst.keyIndex);
            }
        }
    }
    std::sort(readKeys.begin(), readKeys.end());
    readKeys.erase(std::unique(readKeys.begin(), readKeys.end()), readKeys.end());
    if (globalReadKeys) *globalReadKeys = readKeys;

    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        const auto& fn = module.functions[fnIdx];

        brass::il::BronzeFunction bfn;
        bfn.name = (fnIdx < uniqueNames.size() && !uniqueNames[fnIdx].empty())
                       ? uniqueNames[fnIdx]
                       : fn.name;
        bfn.return_type = (fn.returnType == il::Type::Bool)
                              ? brass::il::BronzeType::I32
                              : mapType(fn.returnType);
        bfn.is_exported = fn.isExported;

        for (size_t p = 0; p < fn.params.size(); ++p) {
            bfn.params.push_back({static_cast<uint32_t>(p), mapType(fn.params[p].type)});
        }

        if (fn.blocks.empty()) {
            ast.functions.push_back(std::move(bfn));
            continue;
        }

        for (const auto& block : fn.blocks) {
            brass::il::BronzeBlock bblk;
            bblk.id = block.id;
            bblk.handler_id = (block.handler != il::kNoBlock) ? block.handler : UINT32_MAX;

            for (const auto& p : block.params) {
                bblk.params.push_back({p.id, mapType(p.type)});
            }

            for (const auto& inst : block.instructions) {
                bblk.instructions.push_back(lowerInstruction(inst, module, uniqueNames));
            }

            bfn.blocks.push_back(std::move(bblk));
        }

        ast.functions.push_back(std::move(bfn));
    }

    return ast;
}

} // namespace bronze::codegen
