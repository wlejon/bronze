// Statement lowering: the hoisting statement-list pass, the statement
// dispatcher, and the statements that are not control flow. Control-flow
// statements live in lower_control.cpp.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn) {
    // Function declarations hoist: every one in this list is bound before any
    // statement runs, so code above a declaration can call it. A nested
    // declaration IS a closure — there is no second code path for it.
    for (const auto* stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt);
        if (!fnDecl) continue;
        auto closure = lowerClosure(*fnDecl, fnDecl->name, fnDecl->name, fnDecl->params,
                                    fnDecl->returnType, fnDecl->body, fnDecl->span, ilFn);
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
        // Statements after this list's `return`, `break` or `continue` are
        // unreachable, and there is no block left to put them in: emitting
        // them would append instructions after a terminator, which the
        // verifier rejects — `function f() { return 3; g(); }` failed to
        // compile at all, reporting the verifier's internal wording.
        //
        // Dropping them is the language's answer rather than a convenience:
        // unreachable code has no effect. What a dead region CAN still
        // contribute is hoisting, and the pass above has already done it, so
        // a function declared below a `return` is bound either way.
        if (currentBlockIsTerminated(ilFn)) break;
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
        return lowerSwitchStmt(sw, ilFn);
    }

    if (const auto* labeled = dynamic_cast<const ast::LabeledStmt*>(&stmt)) {
        return lowerLabeledStmt(labeled, ilFn);
    }

    if (const auto* fi = dynamic_cast<const ast::ForInStmt*>(&stmt)) {
        return lowerForInStmt(fi, ilFn);
    }

    if (const auto* fo = dynamic_cast<const ast::ForOfStmt*>(&stmt)) {
        return lowerForOfStmt(fo, ilFn);
    }

    if (const auto* tr = dynamic_cast<const ast::TryStmt*>(&stmt)) {
        return lowerTryStmt(tr, ilFn);
    }

    if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&stmt)) {
        return lowerThrowStmt(th, ilFn);
    }

    if (dynamic_cast<const ast::ExportNamesDecl*>(&stmt)) {
        // `export { a as b };` names bindings and evaluates nothing — all of
        // its effect is in the declaration beside it, which is lowered as an
        // ordinary declaration. The linker removes these before lowering runs
        // on a real build; a test that lowers one parsed file still meets one.
        return true;
    }
    if (dynamic_cast<const ast::ImportDecl*>(&stmt)) {
        // Not the same: an import BINDS a name, and if one reaches lowering
        // then the binding was never resolved to the module that defines it.
        // That is a broken pipeline, not a construct with no effect.
        diags_.error(stmt.span,
                     "internal error: an import declaration reached lowering; the module linker "
                     "did not run");
        return false;
    }

    diags_.error(stmt.span, "unsupported AST node");
    return false;
}

bool Lowerer::lowerVarDecl(const ast::VarDecl* varDecl, il::Function& ilFn) {
    il::ValueId initId = il::kNoValue;
    il::Type declType = il::Type::Dynamic;

    // A destructuring declaration binds several names from one value, so none
    // of the single-name machinery below applies: no annotation (there is no
    // one name to annotate), no proven-number unboxing (the pieces come out of
    // a dynamic read), and an initialiser the parser has already made
    // mandatory.
    if (varDecl->pattern) {
        auto initVal = lowerExpr(*varDecl->init, ilFn);
        if (!initVal) return false;
        PatternTarget target{.declare = true,
                             .isConst = varDecl->isConst,
                             .isLet = !varDecl->isConst && !varDecl->isVar,
                             .isVar = varDecl->isVar};
        return lowerPattern(*varDecl->pattern, boxValueIfNeeded(*initVal, ilFn), target, ilFn);
    }

    if (varDecl->init) {
        std::optional<Value> initVal;

        // A binding initialised from a proven typed-array element, EVERY use
        // of which the scan proves coercing, holds the read as an unboxed
        // f64 — NaN standing for the out-of-bounds `undefined`, which those
        // uses cannot tell apart. Uncaptured `const`/`let` only: an env cell
        // or a try-crossing slot is read by code the scan's argument does
        // not cover.
        if (const auto elemKind = typedElemAccessKind(*varDecl->init);
            elemKind && !varDecl->isVar && memoryNames_.count(varDecl->name) == 0 &&
            typedElemBindingUsesCoerce(varDecl->name, varDecl)) {
            initVal = lowerTypedElemRead(
                static_cast<const ast::IndexAccess&>(*varDecl->init), *elemKind, ilFn);
            if (!initVal) return false;
            declType = il::Type::F64;
        } else {
            // 14.3.1.2 / 14.3.2.1: an anonymous function on the right of a
            // binding takes the binding's name, which is what makes
            // `const f = () => {}` report `f.name === "f"`.
            initVal = lowerNamedEvaluation(*varDecl->init, varDecl->name, ilFn);
            if (!initVal) return false;
            declType = initVal->type;
        }

        // A binding inference proved numeric holds an unboxed f64, so the
        // arithmetic over it needs no unbox per use, the SSA joins it takes
        // part in stay f64, and it carries no GC obligation at all — an f64
        // gets no shadow-stack slot. The unbox here cannot fail: the proof says
        // every value that reaches this initialiser is a number.
        if (initVal->type == il::Type::Dynamic && provenNumber(*varDecl->init)) {
            initVal = unboxValueIfNeeded(*initVal, il::Type::F64, ilFn);
            declType = il::Type::F64;
        }

        // The binding's type comes from the initialiser and the proof above;
        // the annotation does not get to overrule either. Believing it makes
        // `let s: number = "abc"` an `unbox.f64` of a boxed string. The claim
        // is compared against what inference proved reaches this initialiser,
        // and disagreement is a warning, not a cast.
        if (!checkAnnotation(varDecl->typeAnnotation, varDecl->span, varDecl->name,
                             inferredType(*varDecl->init))) {
            return false;
        }
        initId = initVal->id;
    }

    bool isInitialized = varDecl->init != nullptr;
    if (!varDecl->init) {
        // `let x: number;` binds `undefined` at the declaration and may hold a
        // number later, which is `number | undefined` — the exact case
        // collapses to `Dynamic` because the lattice has no union types. So
        // nothing is proven here, and the annotation is reported as unprovable
        // rather than as contradicted by the `undefined` the declaration
        // happens to bind: bronze did not look at the later writes at all, and
        // saying "contradicts" would claim more than it knows.
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
    // In a generator, `return` ends the WALK: 27.5.3.2 makes its value the
    // `value` of the final `{ done: true }` result rather than this function's
    // return value.
    if (generator_) return lowerGeneratorReturn(retStmt, ilFn);
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

        // 14.15.3: the try's completion is [return, V] and the finally runs
        // AFTER V is computed — so the expression is lowered above, then every
        // enclosing `finally` runs, then the value leaves. A `return` inside
        // one of them terminates the block first and wins, which is the whole
        // of "a completion from inside finally overrides the pending one".
        if (!runCleanups(0, ilFn)) return false;
        if (currentBlockIsTerminated(ilFn)) return true;

        il::Instruction inst;
        inst.op = il::Op::Ret;
        inst.type = val->type;
        inst.result = il::kNoValue;
        inst.operands = {val->id};
        emitInst(ilFn, inst);
    } else {
        if (!runCleanups(0, ilFn)) return false;
        if (currentBlockIsTerminated(ilFn)) return true;
        il::Instruction inst;
        inst.op = il::Op::Ret;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        // 14.10.1: `return;` returns UNDEFINED, which is a value — so in a
        // function whose calling convention says it returns one, this is not
        // a void return. Emitting one anyway produced a `ret void` in a
        // function typed to return an i64, which LLVM's verifier rejected
        // and nothing before it did: the early-exit-then-value-return shape
        // (`if (x === undefined) return; ... return v;`) is ordinary, and it
        // failed to compile at all. Falling off the END of the same function
        // already produced the undefined here (see lowerFunctionBody); this
        // is the same rule reached by the other door.
        if (ilFn.returnType != il::Type::Void) {
            il::ValueId undefVal = ilFn.valueCount++;
            il::Instruction constInst;
            constInst.op = il::Op::ConstUndefined;
            constInst.type = il::Type::Dynamic;
            constInst.result = undefVal;
            emitInst(ilFn, constInst);
            Value ret = coerceToType(Value{undefVal, il::Type::Dynamic}, ilFn.returnType, ilFn);
            inst.type = ret.type;
            inst.operands = {ret.id};
        }
        emitInst(ilFn, inst);
    }
    return true;
}

}  // namespace bronze::lower
