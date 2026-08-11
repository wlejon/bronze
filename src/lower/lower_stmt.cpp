// Statement lowering: the hoisting statement-list pass, the statement
// dispatcher, and the statements that are not control flow. Control-flow
// statements live in lower_control.cpp.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn) {
    // Function declarations hoist: every one in this list is bound
    // before any statement runs, so code above a declaration can call
    // it (docs/0007 decision 4). A nested declaration IS a closure —
    // there is no second code path for it.
    for (const auto* stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt);
        if (!fnDecl) continue;
        auto closure = lowerClosure(fnDecl->name, fnDecl->params, fnDecl->returnType,
                                    fnDecl->body, fnDecl->span, ilFn);
        if (!closure) return false;
        if (!declareVariable(fnDecl->name, il::Type::Dynamic, /*isConst=*/false,
                             /*isLet=*/true, /*isVar=*/false, /*isInitialized=*/true,
                             closure->id, fnDecl->span)) {
            return false;
        }
        VarBinding& b = varBindings_[activeVarMap_[fnDecl->name]];
        if (b.inEnv) {
            emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, *closure, ilFn);
        }
    }
    for (const auto* stmt : stmts) {
        if (dynamic_cast<const ast::FunctionDecl*>(stmt)) continue;
        if (!lowerStmt(*stmt, ilFn)) return false;
    }
    return true;
}

bool Lowerer::lowerStmt(const ast::Stmt& stmt, il::Function& ilFn) {
    if (const auto* blockStmt = dynamic_cast<const ast::BlockStmt*>(&stmt)) {
        enterScope(blockStmt->stmts, ilFn);
        std::vector<const ast::Stmt*> stmts;
        for (const auto& s : blockStmt->stmts) stmts.push_back(s.get());
        if (!lowerStmtList(stmts, ilFn)) return false;
        exitScope();
        return true;
    }

    if (const auto* varDecl = dynamic_cast<const ast::VarDecl*>(&stmt)) {
        return lowerVarDecl(varDecl, ilFn);
    }

    if (const auto* retStmt = dynamic_cast<const ast::ReturnStmt*>(&stmt)) {
        return lowerReturnStmt(retStmt, ilFn);
    }

    if (const auto* exprStmt = dynamic_cast<const ast::ExprStmt*>(&stmt)) {
        auto val = lowerExpr(*exprStmt->expr, ilFn);
        if (!val) return false;
        return true;
    }

    if (const auto* ifStmt = dynamic_cast<const ast::IfStmt*>(&stmt)) {
        return lowerIfStmt(ifStmt, ilFn);
    }

    if (const auto* whileStmt = dynamic_cast<const ast::WhileStmt*>(&stmt)) {
        return lowerWhileStmt(whileStmt, ilFn);
    }

    if (const auto* doWhileStmt = dynamic_cast<const ast::DoWhileStmt*>(&stmt)) {
        return lowerDoWhileStmt(doWhileStmt, ilFn);
    }

    if (const auto* forStmt = dynamic_cast<const ast::ForStmt*>(&stmt)) {
        return lowerForStmt(forStmt, ilFn);
    }

    if (const auto* breakStmt = dynamic_cast<const ast::BreakStmt*>(&stmt)) {
        return lowerBreakStmt(breakStmt, ilFn);
    }

    if (const auto* continueStmt = dynamic_cast<const ast::ContinueStmt*>(&stmt)) {
        return lowerContinueStmt(continueStmt, ilFn);
    }

    if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&stmt)) {
        diags_.error(sw->span, "unsupported construct: switch statement");
        return false;
    }

    if (const auto* fi = dynamic_cast<const ast::ForInStmt*>(&stmt)) {
        diags_.error(fi->span, "unsupported construct: for-in loop");
        return false;
    }

    if (const auto* fo = dynamic_cast<const ast::ForOfStmt*>(&stmt)) {
        diags_.error(fo->span, "unsupported construct: for-of loop");
        return false;
    }

    if (const auto* tr = dynamic_cast<const ast::TryStmt*>(&stmt)) {
        diags_.error(tr->span, "unsupported construct: try/catch/throw");
        return false;
    }

    if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&stmt)) {
        diags_.error(th->span, "unsupported construct: try/catch/throw");
        return false;
    }

    diags_.error(stmt.span, "unsupported AST node");
    return false;
}

bool Lowerer::lowerVarDecl(const ast::VarDecl* varDecl, il::Function& ilFn) {
    il::ValueId initId = il::kNoValue;
    il::Type declType = il::Type::Dynamic;

    if (varDecl->init) {
        auto initVal = lowerExpr(*varDecl->init, ilFn);
        if (!initVal) return false;
        declType = initVal->type;

        if (!varDecl->typeAnnotation.empty()) {
            auto annType = mapTypeAnnotation(varDecl->typeAnnotation, varDecl->span, diags_);
            if (!annType) return false;
            if (*annType == il::Type::Dynamic && initVal->type != il::Type::Dynamic) {
                initVal = boxValueIfNeeded(*initVal, ilFn);
            } else if (*annType != il::Type::Dynamic && initVal->type == il::Type::Dynamic) {
                initVal = unboxValueIfNeeded(*initVal, *annType, ilFn);
            }
            declType = *annType;
        }
        initId = initVal->id;
    }

    bool isInitialized = varDecl->init != nullptr;
    if (!varDecl->init) {
        if (!varDecl->typeAnnotation.empty()) {
            auto annType = mapTypeAnnotation(varDecl->typeAnnotation, varDecl->span, diags_);
            if (!annType) return false;
            declType = *annType;
        }
        if (!varDecl->isConst && declType == il::Type::Dynamic) {
            // JS: `let x;` / `var x;` binds undefined right here —
            // the TDZ ends at the declaration, not at the first
            // assignment. (Annotated typed slots keep the stricter
            // read-before-assign error: undefined has no typed form.)
            il::ValueId undefId = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstUndefined;
            inst.type = il::Type::Dynamic;
            inst.result = undefId;
            emitInst(ilFn, inst);
            initId = undefId;
            isInitialized = true;
        }
    }

    bool isConst = varDecl->isConst;
    bool isVar = varDecl->isVar;
    bool isLet = !isConst && !isVar;

    if (!declareVariable(varDecl->name, declType, isConst, isLet, isVar, isInitialized,
                         initId, varDecl->span)) {
        return false;
    }
    // A captured declaration's initial value has to reach its
    // environment slot; the SSA value alone is invisible to any
    // closure over it.
    VarBinding& bound = varBindings_[activeVarMap_[varDecl->name]];
    if (bound.inEnv && isInitialized && initId != il::kNoValue) {
        emitEnvSet(envDepthOf(bound.envScopeIndex), bound.envSlot,
                   Value{initId, declType}, ilFn);
    }
    return true;
}

bool Lowerer::lowerReturnStmt(const ast::ReturnStmt* retStmt, il::Function& ilFn) {
    if (retStmt->value) {
        auto val = lowerExpr(*retStmt->value, ilFn);
        if (!val) return false;

        if (ilFn.returnType == il::Type::Void) {
            ilFn.returnType = val->type;
        } else if (ilFn.returnType == il::Type::Dynamic) {
            val = boxValueIfNeeded(*val, ilFn);
        } else if (val->type == il::Type::Dynamic) {
            val = unboxValueIfNeeded(*val, ilFn.returnType, ilFn);
        }

        il::Instruction inst;
        inst.op = il::Op::Ret;
        inst.type = val->type;
        inst.result = il::kNoValue;
        inst.operands = {val->id};
        emitInst(ilFn, inst);
    } else {
        il::Instruction inst;
        inst.op = il::Op::Ret;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        emitInst(ilFn, inst);
    }
    return true;
}

}  // namespace bronze::lower
