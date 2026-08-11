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
        auto closure = lowerClosure(*fnDecl, fnDecl->name, fnDecl->params, fnDecl->returnType,
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

    if (const auto* classDecl = dynamic_cast<const ast::ClassDecl*>(&stmt)) {
        return lowerClassDecl(classDecl, ilFn);
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
        return lowerForOfStmt(fo, ilFn);
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

        // A binding inference proved numeric holds an unboxed f64
        // (docs/0010 decision 3), so the arithmetic over it needs no
        // unbox per use, the SSA joins it takes part in stay f64, and it
        // carries no GC obligation at all — an f64 gets no shadow-stack
        // slot (docs/0006 decision 1). The unbox here cannot fail: the
        // proof says every value that reaches this initialiser is a
        // number.
        if (initVal->type == il::Type::Dynamic && provenNumber(*varDecl->init)) {
            initVal = unboxValueIfNeeded(*initVal, il::Type::F64, ilFn);
            declType = il::Type::F64;
        }

        // The binding's type comes from the initialiser and the proof above;
        // the annotation does not get to overrule either (docs/0010 decision
        // 6). Believing it makes `let s: number = "abc"` an `unbox.f64` of a
        // boxed string. The claim is compared against what inference proved
        // reaches this initialiser, and disagreement is a warning, not a cast.
        if (!checkAnnotation(varDecl->typeAnnotation, varDecl->span, varDecl->name,
                             inferredType(*varDecl->init))) {
            return false;
        }
        initId = initVal->id;
    }

    bool isInitialized = varDecl->init != nullptr;
    if (!varDecl->init) {
        // `let x: number;` binds `undefined` at the declaration and may hold
        // a number later, which is `number | undefined` — the exact case
        // docs/0010 decision 2 says collapses to `Dynamic` because there are
        // no union types. So nothing is proven here, and the annotation is
        // reported as unprovable rather than as contradicted by the
        // `undefined` the declaration happens to bind: bronze did not look
        // at the later writes at all, and saying "contradicts" would claim
        // more than it knows.
        if (!checkAnnotation(varDecl->typeAnnotation, varDecl->span, varDecl->name,
                             types::Type::dynamic())) {
            return false;
        }
        if (!varDecl->isConst && declType == il::Type::Dynamic) {
            // JS: `let x;` / `var x;` binds undefined right here — the TDZ
            // ends at the declaration, not at the first assignment. Which is
            // why `declType` is `Dynamic` for every uninitialised
            // declaration: a typed slot has no room for `undefined`.
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
