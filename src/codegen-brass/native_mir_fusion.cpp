#include "codegen-brass/native_mir_fusion.h"

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend_sections.h"
#include "embed/embed_native.h"

#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/cross_module.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/instruction.hpp>
#include <brass/mir/opcodes.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bronze::codegen {

namespace {

brass::Value* emitThreadTableAddr(brass::Builder& b, const std::string& entrySymbol,
                                  const std::string& tableSym, bool perThread) {
    brass::Value* addr = b.build_func_addr(tableSym);
    if (!perThread) return addr;
    brass::Value* tls = b.build_pinned_tls_read();
    brass::Value* deltas = b.build_load(brass::Type::i64(), tls, BRONZE_TLS_MODULE_DELTAS_OFF);
    const std::string slotSym = codegen::moduleSymbolName(entrySymbol, "__bronze_module_slot");
    brass::Value* slotCell = b.build_func_addr(slotSym);
    brass::Value* slot = b.build_load(brass::Type::i64(), slotCell, 0);
    brass::Value* entry = b.build_add(deltas, b.build_shl(slot, b.build_iconst_i64(3)));
    brass::Value* delta = b.build_load(brass::Type::i64(), entry, 0);
    return b.build_add(addr, delta);
}

bool mirTypesMatch(const brass::Function& srcFn,
                   const std::vector<brass::Type>& paramTypes,
                   brass::Type retType) {
    if (srcFn.param_types().size() != paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (srcFn.param_types()[i] != paramTypes[i]) return false;
    }
    return srcFn.return_type() == retType;
}

}  // namespace

bool tryEmitMirNativeThunk(
    brass::Module& mod,
    brass::Function*& thunk,
    const std::string& importName,
    const std::string& importSignature,
    bool boolReturn,
    const std::vector<brass::Type>& paramTypes,
    brass::Type retType,
    const std::string& entrySymbol,
    const std::string& importsSymbol,
    int32_t slotOffset,
    bool perThread) {

    const size_t sp = importName.find(' ');
    if (sp == std::string::npos) return false;
    const std::string kindText = importName.substr(0, sp);
    const std::string path = importName.substr(sp + 1);
    if (kindText == "class") return false;

    embed::NativeMirSpec spec;
    if (!embed::lookupNativeMir(kindText, path, importSignature, spec)) return false;

    const brass::Function* srcFn = spec.mirModule->get_function(spec.mirFunction);
    if (!srcFn || !srcFn->has_body()) return false;
    if (!mirTypesMatch(*srcFn, paramTypes, retType)) return false;

    brass::CrossDomainFusionOptions fopts;
    fopts.allow_nanbox_unboxing = false;
    fopts.max_inlined_insts = spec.maxInlinedInsts > 0 ? spec.maxInlinedInsts : 8192;
    fopts.audit_opts.require_no_gc = true;
    fopts.audit_opts.require_no_pinned_tls = true;
    fopts.audit_opts.require_no_alloc = true;
    fopts.audit_opts.require_no_deopt = true;
    fopts.audit_opts.require_no_eh = true;

    const brass::DomainAuditReport preAudit =
        brass::audit_function_domain(*srcFn, *spec.mirModule, fopts.audit_opts);
    if (!preAudit.ok) return false;

    const std::string thunkName(thunk->name());
    brass::Builder b(mod);
    b.set_function(thunk);
    brass::BasicBlock* entryBb = b.append_block("entry");
    std::vector<brass::Value*> args;
    args.reserve(paramTypes.size());
    for (const auto& t : paramTypes) {
        args.push_back(b.add_block_param(entryBb, t));
    }

    brass::BasicBlock* fusedBb = b.append_block("fused_entry");
    brass::BasicBlock* slowBb = nullptr;
    if (spec.epochPtr != nullptr) {
        slowBb = b.append_block("slow");
        slowBb->set_cold();
    }

    b.position_at_end(entryBb);
    if (slowBb) {
        brass::Value* addr = b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(spec.epochPtr)));
        brass::Value* curEpoch = b.build_load(brass::Type::i64(), addr, 0);
        brass::Value* wantEpoch = b.build_iconst_i64(static_cast<int64_t>(spec.expectedEpoch));
        brass::Value* ok = b.build_eq(curEpoch, wantEpoch);
        b.build_br_if(ok, fusedBb, slowBb);

        b.position_at_end(slowBb);
        brass::Value* table = emitThreadTableAddr(b, entrySymbol, importsSymbol, perThread);
        brass::Value* callee = b.build_load(brass::Type::ptr(), table, slotOffset);
        brass::Value* slowRes = b.build_call_indirect(
            callee, retType, brass::Span<brass::Value* const>(args.data(), args.size()));
        if (retType.is_void()) {
            b.build_ret_void();
        } else if (boolReturn) {
            b.build_ret(b.build_and(slowRes, b.build_iconst_i32(1)));
        } else {
            b.build_ret(slowRes);
        }
    } else {
        b.build_br(fusedBb);
    }

    b.position_at_end(fusedBb);
    brass::Value* callVal = b.build_call(
        spec.mirFunction, retType, brass::Span<brass::Value* const>(args.data(), args.size()));
    brass::Instruction* callInst = nullptr;
    for (brass::Instruction* inst : *fusedBb) {
        if (inst && inst->opcode() == brass::Opcode::call) {
            callInst = inst;
        }
    }
    if (!callInst) {
        mod.remove_function(thunk);
        thunk = mod.create_function(
            thunkName, retType, brass::Span<const brass::Type>(paramTypes.data(), paramTypes.size()));
        return false;
    }

    if (retType.is_void()) {
        b.build_ret_void();
    } else if (boolReturn) {
        b.build_ret(b.build_and(callVal, b.build_iconst_i32(1)));
    } else {
        b.build_ret(callVal);
    }

    thunk->rebuild_cfg_predecessors();
    fopts.link_opts.symbol_prefix = thunkName + "_";
    const brass::CrossDomainFusionResult fres = brass::fuse_cross_module_call(
        *thunk, callInst, mod, *spec.mirModule, spec.mirFunction, fopts);
    if (!fres.fused) {
        mod.remove_function(thunk);
        thunk = mod.create_function(
            thunkName, retType, brass::Span<const brass::Type>(paramTypes.data(), paramTypes.size()));
        return false;
    }

    thunk->rebuild_cfg_predecessors();
    return true;
}

}  // namespace bronze::codegen
