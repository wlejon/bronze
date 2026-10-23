#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "support/source.h"

namespace bronze::gpu_detail {

inline bool isGpuBuiltinName(std::string_view name) {
    return name == "tid_x" || name == "tid_y" || name == "tid_z" ||
           name == "ctaid_x" || name == "ctaid_y" || name == "ctaid_z" ||
           name == "ntid_x" || name == "ntid_y" || name == "ntid_z" ||
           name == "nctaid_x" || name == "nctaid_y" || name == "nctaid_z" ||
           name == "global_tid_x" || name == "lane_id" || name == "warp_id" ||
           name == "sync" || name == "bar_sync";
}

inline bool isAllowedMathMethod(std::string_view name) {
    return name == "sin" || name == "cos" || name == "sqrt" || name == "abs" ||
           name == "floor" || name == "ceil" || name == "round" || name == "trunc" ||
           name == "exp" || name == "log" || name == "min" || name == "max" ||
           name == "pow" || name == "atan2" || name == "imul" || name == "fround";
}

inline bool isAllowedMathConst(std::string_view name, float* outVal = nullptr) {
    float v = 0.0f;
    if (name == "PI") v = 3.14159265358979323846f;
    else if (name == "E") v = 2.71828182845904523536f;
    else if (name == "LN2") v = 0.69314718055994530942f;
    else if (name == "LN10") v = 2.30258509299404568402f;
    else if (name == "LOG2E") v = 1.44269504088896340736f;
    else if (name == "LOG10E") v = 0.43429448190325182765f;
    else if (name == "SQRT2") v = 1.41421356237309504880f;
    else if (name == "SQRT1_2") v = 0.70710678118654752440f;
    else return false;
    if (outVal) *outVal = v;
    return true;
}

inline std::string formatDiag(const std::string& file, const LineTable& lt, Span span,
                              const std::string& fnName, const std::string& msg) {
    SourceBuffer::LineCol lc = lt.lineCol(span.begin);
    return file + ":" + std::to_string(lc.line) + ":" + std::to_string(lc.column) +
           ": error: GPU kernel '" + fnName + "': " + msg;
}

struct AstSubsetChecker {
    const std::string& file;
    const LineTable& lt;
    const std::string& fnName;
    std::unordered_set<std::string> paramNames;
    std::unordered_set<std::string> localNames;
    std::vector<std::string>& diags;

    void error(Span span, const std::string& msg) {
        diags.push_back(formatDiag(file, lt, span, fnName, msg));
    }

    void checkExpr(const ast::Expr* expr) {
        if (!expr) return;
        if (dynamic_cast<const ast::NumberLit*>(expr) ||
            dynamic_cast<const ast::BoolLit*>(expr)) return;
        if (dynamic_cast<const ast::ObjectLit*>(expr)) {
            return error(expr->span, "object allocation ('{...}') is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::ArrayLit*>(expr)) {
            return error(expr->span, "dynamic array allocation ('[...]') is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::NewExpr*>(expr)) {
            return error(expr->span, "constructor allocation ('new') is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::FunctionExpr*>(expr)) {
            return error(expr->span, "closure/function creation is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::StringLit*>(expr) ||
            dynamic_cast<const ast::TemplateLit*>(expr) ||
            dynamic_cast<const ast::TaggedTemplate*>(expr) ||
            dynamic_cast<const ast::RegExpLit*>(expr) ||
            dynamic_cast<const ast::BigIntLit*>(expr)) {
            return error(expr->span, "non-numeric literal (string/template/regexp/BigInt) is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::YieldExpr*>(expr) ||
            dynamic_cast<const ast::DynamicImportExpr*>(expr)) {
            return error(expr->span, "async/generator suspension is not allowed in @kernel functions");
        }
        if (dynamic_cast<const ast::ThisExpr*>(expr) ||
            dynamic_cast<const ast::SuperCall*>(expr) ||
            dynamic_cast<const ast::SuperMember*>(expr)) {
            return error(expr->span, "'this'/'super' receiver access is not allowed in @kernel functions");
        }
        if (const auto* id = dynamic_cast<const ast::Ident*>(expr)) {
            if (!paramNames.count(id->name) && !localNames.count(id->name) &&
                id->name != "Math" && id->name != "NaN" && id->name != "Infinity" &&
                !isGpuBuiltinName(id->name)) {
                error(id->span, "host/DOM global or outer variable '" + id->name +
                                    "' is not allowed in @kernel functions");
            }
            return;
        }
        if (const auto* un = dynamic_cast<const ast::Unary*>(expr)) {
            if (un->op == ast::UnaryOp::Delete || un->op == ast::UnaryOp::TypeOf) {
                error(un->span, "dynamic unary operator is not allowed in @kernel functions");
            }
            return checkExpr(un->operand.get());
        }
        if (const auto* bin = dynamic_cast<const ast::Binary*>(expr)) {
            if (bin->op == ast::BinaryOp::In || bin->op == ast::BinaryOp::InstanceOf) {
                error(bin->span, "dynamic relational operator ('in'/'instanceof') is not allowed in @kernel functions");
            }
            if (ast::isAssignOp(bin->op)) {
                if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
                    error(mem->span, "property mutation ('." + mem->property + " = ...') is not allowed in @kernel functions");
                }
            }
            checkExpr(bin->lhs.get());
            return checkExpr(bin->rhs.get());
        }
        if (const auto* tern = dynamic_cast<const ast::Ternary*>(expr)) {
            checkExpr(tern->condition.get());
            checkExpr(tern->thenExpr.get());
            return checkExpr(tern->elseExpr.get());
        }
        if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(expr)) {
            checkExpr(idx->object.get());
            return checkExpr(idx->index.get());
        }
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(expr)) {
            if (const auto* objId = dynamic_cast<const ast::Ident*>(mem->object.get())) {
                if (objId->name == "Math") {
                    if (!isAllowedMathConst(mem->property) && !isAllowedMathMethod(mem->property)) {
                        error(mem->span, "unsupported Math member 'Math." + mem->property + "' in @kernel function");
                    }
                    return;
                }
                if (paramNames.count(objId->name) &&
                    (mem->property == "length" || mem->property == "byteLength")) {
                    return;
                }
            }
            checkExpr(mem->object.get());
            return error(mem->span, "dynamic property access '." + mem->property + "' is not allowed in @kernel functions");
        }
        if (const auto* call = dynamic_cast<const ast::Call*>(expr)) {
            if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
                if (const auto* objId = dynamic_cast<const ast::Ident*>(mem->object.get())) {
                    if (objId->name == "Math" && isAllowedMathMethod(mem->property)) {
                        for (const auto& a : call->args) checkExpr(a.get());
                        return;
                    }
                }
            }
            if (const auto* calleeId = dynamic_cast<const ast::Ident*>(call->callee.get())) {
                if (isGpuBuiltinName(calleeId->name)) {
                    for (const auto& a : call->args) checkExpr(a.get());
                    return;
                }
            }
            checkExpr(call->callee.get());
            for (const auto& a : call->args) checkExpr(a.get());
            return error(call->span, "arbitrary function/method call is not allowed in @kernel functions");
        }
        error(expr->span, "unsupported expression in @kernel function");
    }

    void checkStmt(const ast::Stmt* stmt) {
        if (!stmt) return;
        if (const auto* vd = dynamic_cast<const ast::VarDecl*>(stmt)) {
            if (vd->pattern) error(vd->span, "destructuring pattern is not allowed in @kernel functions");
            else if (!vd->name.empty()) localNames.insert(vd->name);
            return checkExpr(vd->init.get());
        }
        if (const auto* blk = dynamic_cast<const ast::BlockStmt*>(stmt)) {
            for (const auto& s : blk->stmts) checkStmt(s.get());
            return;
        }
        if (const auto* es = dynamic_cast<const ast::ExprStmt*>(stmt)) return checkExpr(es->expr.get());
        if (const auto* ret = dynamic_cast<const ast::ReturnStmt*>(stmt)) return checkExpr(ret->value.get());
        if (const auto* ifs = dynamic_cast<const ast::IfStmt*>(stmt)) {
            checkExpr(ifs->condition.get());
            for (const auto& s : ifs->thenBody) checkStmt(s.get());
            for (const auto& s : ifs->elseBody) checkStmt(s.get());
            return;
        }
        if (const auto* ws = dynamic_cast<const ast::WhileStmt*>(stmt)) {
            checkExpr(ws->condition.get());
            for (const auto& s : ws->body) checkStmt(s.get());
            return;
        }
        if (const auto* dws = dynamic_cast<const ast::DoWhileStmt*>(stmt)) {
            for (const auto& s : dws->body) checkStmt(s.get());
            return checkExpr(dws->condition.get());
        }
        if (const auto* fs = dynamic_cast<const ast::ForStmt*>(stmt)) {
            for (const auto& s : fs->init) checkStmt(s.get());
            checkExpr(fs->condition.get());
            checkExpr(fs->update.get());
            for (const auto& s : fs->body) checkStmt(s.get());
            return;
        }
        if (dynamic_cast<const ast::BreakStmt*>(stmt) ||
            dynamic_cast<const ast::ContinueStmt*>(stmt)) return;
        if (dynamic_cast<const ast::ThrowStmt*>(stmt) ||
            dynamic_cast<const ast::TryStmt*>(stmt)) {
            return error(stmt->span, "exceptions ('throw'/'try'/'catch') are not allowed in @kernel functions");
        }
        error(stmt->span, "unsupported statement in @kernel function");
    }
};

inline const ast::FunctionDecl* findTargetKernelDecl(const ast::Module& mod,
                                                     const std::string& sourceText,
                                                     const std::string& requestedName) {
    const ast::FunctionDecl* firstFn = nullptr;
    for (const auto& stmtPtr : mod.body) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmtPtr.get());
        if (!fnDecl) continue;
        if (!firstFn) firstFn = fnDecl;
        if (!requestedName.empty()) {
            if (fnDecl->name == requestedName) return fnDecl;
            continue;
        }
        uint32_t endPos = std::min<uint32_t>(fnDecl->span.begin, static_cast<uint32_t>(sourceText.size()));
        uint32_t startPos = endPos > 200 ? endPos - 200 : 0;
        std::string_view window(sourceText.data() + startPos, endPos - startPos);
        if (window.find("@kernel") != std::string_view::npos) return fnDecl;
    }
    return requestedName.empty() ? firstFn : nullptr;
}

// Simplify guarded regions on a copy of `il::Function`: fold `is.number` and
// `is.dense_array` branches to their fast `target`, prune unreachable `Slow`
// blocks, fuse single-predecessor jump chains, and unify multiple `ret` blocks.
inline il::Function normalizeGpuIlFunction(const il::Function& orig) {
    il::Function fn = orig;
    std::unordered_set<il::ValueId> guardConds;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::IsNumber || inst.op == il::Op::IsDenseArray) {
                guardConds.insert(inst.result);
            }
        }
    }

    for (auto& blk : fn.blocks) {
        if (blk.instructions.empty()) continue;
        auto& term = blk.instructions.back();
        if (term.op == il::Op::Branch && !term.operands.empty()) {
            bool isGuard = guardConds.count(term.operands[0]) != 0;
            if (!isGuard && term.elseTarget.block < fn.blocks.size() &&
                fn.blocks[term.elseTarget.block].copyClass == il::CopyClass::Slow &&
                blk.copyClass != il::CopyClass::Slow) {
                isGuard = true;
            }
            if (isGuard) {
                term.op = il::Op::Jump;
                term.operands.clear();
                term.elseTarget = {};
            }
        }
    }

    auto pruneUnreachable = [&]() {
        if (fn.blocks.empty()) return;
        std::vector<bool> vis(fn.blocks.size(), false);
        std::vector<uint32_t> q = {0};
        vis[0] = true;
        for (size_t qi = 0; qi < q.size(); ++qi) {
            uint32_t b = q[qi];
            if (fn.blocks[b].instructions.empty()) continue;
            const auto& term = fn.blocks[b].instructions.back();
            if (term.op == il::Op::Jump && term.target.block < fn.blocks.size() && !vis[term.target.block]) {
                vis[term.target.block] = true;
                q.push_back(term.target.block);
            } else if (term.op == il::Op::Branch) {
                if (term.target.block < fn.blocks.size() && !vis[term.target.block]) {
                    vis[term.target.block] = true;
                    q.push_back(term.target.block);
                }
                if (term.elseTarget.block < fn.blocks.size() && !vis[term.elseTarget.block]) {
                    vis[term.elseTarget.block] = true;
                    q.push_back(term.elseTarget.block);
                }
            }
        }
        std::vector<uint32_t> remap(fn.blocks.size(), il::kNoBlock);
        std::vector<il::Block> kept;
        for (size_t i = 0; i < fn.blocks.size(); ++i) {
            if (vis[i]) {
                remap[i] = static_cast<uint32_t>(kept.size());
                kept.push_back(std::move(fn.blocks[i]));
                kept.back().id = remap[i];
            }
        }
        for (auto& blk : kept) {
            if (blk.instructions.empty()) continue;
            auto& term = blk.instructions.back();
            if (term.target.block < remap.size()) term.target.block = remap[term.target.block];
            if (term.elseTarget.block < remap.size()) term.elseTarget.block = remap[term.elseTarget.block];
        }
        fn.blocks = std::move(kept);
    };

    pruneUnreachable();

    std::unordered_map<il::ValueId, il::ValueId> valAlias;
    auto resolveAlias = [&](il::ValueId v) {
        while (valAlias.count(v)) v = valAlias[v];
        return v;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<uint32_t> inDeg(fn.blocks.size(), 0);
        for (const auto& blk : fn.blocks) {
            if (blk.instructions.empty()) continue;
            const auto& term = blk.instructions.back();
            if (term.op == il::Op::Jump && term.target.block < fn.blocks.size()) {
                inDeg[term.target.block]++;
            } else if (term.op == il::Op::Branch) {
                if (term.target.block < fn.blocks.size()) inDeg[term.target.block]++;
                if (term.elseTarget.block < fn.blocks.size()) inDeg[term.elseTarget.block]++;
            }
        }
        for (size_t a = 0; a < fn.blocks.size(); ++a) {
            if (fn.blocks[a].instructions.empty()) continue;
            auto term = fn.blocks[a].instructions.back();
            if (term.op != il::Op::Jump) continue;
            uint32_t b = term.target.block;
            if (b == 0 || b == a || b >= fn.blocks.size() || inDeg[b] != 1) continue;

            for (size_t k = 0; k < fn.blocks[b].params.size() && k < term.target.args.size(); ++k) {
                valAlias[fn.blocks[b].params[k].id] = resolveAlias(term.target.args[k]);
            }
            fn.blocks[a].instructions.pop_back();
            for (auto& inst : fn.blocks[b].instructions) {
                fn.blocks[a].instructions.push_back(std::move(inst));
            }
            fn.blocks[b].instructions.clear();
            changed = true;
            break;
        }
        if (changed) pruneUnreachable();
    }

    for (auto& blk : fn.blocks) {
        for (auto& inst : blk.instructions) {
            for (auto& op : inst.operands) op = resolveAlias(op);
            for (auto& arg : inst.target.args) arg = resolveAlias(arg);
            for (auto& arg : inst.elseTarget.args) arg = resolveAlias(arg);
        }
    }

    std::vector<size_t> retBlocks;
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        if (!fn.blocks[i].instructions.empty() &&
            fn.blocks[i].instructions.back().op == il::Op::Ret) {
            retBlocks.push_back(i);
        }
    }
    if (retBlocks.size() > 1) {
        uint32_t exitId = static_cast<uint32_t>(fn.blocks.size());
        il::Block exitBlk;
        exitBlk.id = exitId;
        il::Instruction retInst;
        retInst.op = il::Op::Ret;
        retInst.type = il::Type::Void;
        exitBlk.instructions.push_back(retInst);
        for (size_t bi : retBlocks) {
            auto& term = fn.blocks[bi].instructions.back();
            term.op = il::Op::Jump;
            term.operands.clear();
            term.target.block = exitId;
            term.target.args.clear();
        }
        fn.blocks.push_back(std::move(exitBlk));
    }

    return fn;
}

}  // namespace bronze::gpu_detail
