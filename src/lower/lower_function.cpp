#include <algorithm>
#include <string>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// The closure-parameter plan is made for THIS body and consumed while THIS body
// is lowered, so it lives in a frame that opens and closes here. Keyed by node
// address, it must not survive the nodes it names — a class constructor's body
// is a copy that dies with `lowerClass`, and an entry outliving that copy is
// later answered for whatever the allocator puts at the same address. See the
// note on `provenClosureParams_`.
bool Lowerer::lowerFunctionBody(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                                bool isGenerator, bool isAsync) {
    bool oldUserFn = inUserFunction_;
    inUserFunction_ = true;
    provenClosureParams_.emplace_back();
    const bool ok = lowerBodyWithPlan(params, body, ilFn, isGenerator, isAsync);
    provenClosureParams_.pop_back();
    inUserFunction_ = oldUserFn;
    return ok;
}

bool Lowerer::lowerBodyWithPlan(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                                bool isGenerator, bool isAsync) {
    // Bodies lower one at a time (the state resets below assume it), so a
    // plain pointer is the whole bookkeeping the typed-element binding scan
    // needs.
    currentBodyStmts_ = &body;
    // Which of this body's own nested declarations have parameters every call
    // site proves to be Numbers (lower_scope.cpp). Decided BEFORE any statement
    // of the body is lowered, because the first thing lowering does with a
    // `function f() {}` statement is build `f`'s IL skeleton — parameter types
    // and all — and the whole point of the plan is to be part of it.
    planClosureParamNumbers(params, body);
    ilFn.blocks.push_back(il::Block{.id = 0});
    ilFn.isStrict = strictCode_;
    ilFn.isGenerator = isGenerator;
    currentBlockIdx_ = 0;
    varBindings_.clear();
    activeVarMap_.clear();
    currentScopeDepth_ = 0;
    varDeclCounter_ = 0;
    jumpStack_.clear();
    scopeHasEnv_.clear();
    immutableEnvCache_.clear();
    cachedTypedElemGet_.reset();
    functionVarNames_ = ast::getHoistedVarDeclarations(body);
    for (auto& n : ast::getAssignedNames(body)) assignedNames_.insert(std::move(n));

    // Synthetic parameters lead: [__env?][__this?] then source params.
    const uint32_t paramBase = static_cast<uint32_t>(ilFn.firstSourceParam());
    if (ilFn.needsEnv) {
        // A closure: its environment arrives as the first parameter, and the
        // chain from there already reaches every enclosing scope including the
        // module's.
        currentEnvValue_ = 0;
    } else {
        // A module function. It has no environment parameter and never will —
        // that is what keeps it a direct-call target — so the one scope it can
        // still need, the module's, is loaded from the runtime.
        currentEnvValue_ =
            referencesModuleEnv(params, body) ? emitModuleEnvGet(ilFn) : il::kNoValue;
    }
    entryEnvValue_ = currentEnvValue_;
    currentThisValue_ = ilFn.needsThis ? (ilFn.needsEnv ? 1u : 0u) : il::kNoValue;

    std::vector<const ast::Stmt*> stmts;
    stmts.reserve(body.size());
    for (const auto& s : body) stmts.push_back(s.get());
    enterFunctionEnv(params, stmts, ilFn, isGenerator, isAsync);

    // An arrow in this body reads the receiver out of the environment, so
    // the receiver has to be IN it: copy `__this` across on entry, once,
    // exactly as a captured parameter is copied below. Undefined where
    // there is no receiver, which is what `this` means at module level.
    if (functionEnvScope_ != SIZE_MAX && envScopes_[functionEnvScope_].slotOf.contains("this")) {
        Value thisVal{currentThisValue_, il::Type::Dynamic};
        if (currentThisValue_ == il::kNoValue) {
            il::ValueId undef = ilFn.valueCount++;
            il::Instruction undefInst;
            undefInst.op = il::Op::ConstUndefined;
            undefInst.type = il::Type::Dynamic;
            undefInst.result = undef;
            emitInst(ilFn, undefInst);
            thisVal = Value{undef, il::Type::Dynamic};
        }
        emitEnvSet(envDepthOf(functionEnvScope_), envScopes_[functionEnvScope_].slotOf.at("this"),
                   thisVal, ilFn);
    }

    // The arguments object is a BINDING named `arguments`, not a keyword:
    // that is what makes an arrow in this body see it through the ordinary
    // capture machinery, and what makes a `let arguments` shadow it without a
    // rule of its own. Declared before the parameters so that any real
    // declaration of the name — which `ast::usesArguments` already refuses to
    // create this for — would shadow rather than collide with it.
    if (ilFn.needsArguments) {
        const il::ValueId argsVal = static_cast<il::ValueId>(ilFn.firstSourceParam() - 1);
        if (functionEnvScope_ != SIZE_MAX &&
            envScopes_[functionEnvScope_].slotOf.contains("arguments")) {
            emitEnvSet(envDepthOf(functionEnvScope_),
                       envScopes_[functionEnvScope_].slotOf.at("arguments"),
                       Value{argsVal, il::Type::Dynamic}, ilFn);
        }
        if (!declareVariable("arguments", il::Type::Dynamic, /*isConst=*/false, /*isLet=*/false,
                             /*isVar=*/true, /*isInitialized=*/true, argsVal, Span{})) {
            return false;
        }
    }

    if (!lowerParamBindings(params, paramBase, ilFn)) return false;

    const auto allHoistedVars = ast::getHoistedVarDeclarations(body);
    for (const auto& varName : allHoistedVars) {
        if (activeVarMap_.find(varName) == activeVarMap_.end()) {
            il::ValueId undefVal = emitConstUndefined(ilFn);
            if (!declareVariable(varName, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/false,
                                 /*isVar=*/true, /*isInitialized=*/true, undefVal, Span{})) {
                return false;
            }
            VarBinding& b = varBindings_[activeVarMap_[varName]];
            if (b.inEnv) {
                emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot,
                           Value{undefVal, il::Type::Dynamic}, ilFn);
            }
        }
    }
    functionVarNames_.clear();

    // A generator's body does not run here at all (15.5.3): what is left of
    // this function is to close the resume function over the frame the
    // prologue above has just filled in, and hand back the generator object.
    // Its lexical bindings are opened in the resume function's start block, for
    // the reason recorded there. An async function's tail differs in one
    // fact — 27.7.5.1 runs the body synchronously to the first await — and
    // that fact lives in the runtime driver its tail calls, not here.
    if (isGenerator || isAsync) {
        const bool ok = (isGenerator && isAsync) ? lowerAsyncGeneratorTail(stmts, ilFn)
                        : isGenerator             ? lowerGeneratorTail(stmts, ilFn)
                                                  : lowerAsyncTail(stmts, ilFn);
        if (functionEnvScope_ != SIZE_MAX) {
            envScopes_.pop_back();
            currentEnvValue_ = savedEnvValues_.back();
            savedEnvValues_.pop_back();
        }
        return ok;
    }

    // After the parameters, so that a body that redeclares one is still the
    // redeclaration error it was rather than a parameter slot holding the
    // uninitialized marker; before the statements, because 14.3.1 creates the
    // binding when the scope is entered and the declaration only initializes
    // it.
    if (functionEnvScope_ != SIZE_MAX) {
        openLexicalBindings(functionEnvScope_, ast::getLexicalDeclarations(stmts),
                            ast::getDefinitelyAssignedLexicalNames(stmts, &params),
                            ast::getConstDeclarations(stmts), ilFn);
    }

    if (!lowerStmtList(stmts, ilFn)) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        if (currentBlockIdx_ < ilFn.blocks.size()) {
            // A tail block no edge targets (e.g. the join of an if whose
            // arms both return) is unreachable; give it any well-typed
            // ret. A reachable tail means the function can actually fall
            // off the end, which yields undefined.
            bool reachable = currentBlockIdx_ == 0;
            for (const auto& block : ilFn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                        if (inst.target.block == currentBlockIdx_ ||
                            (inst.op == il::Op::Branch && inst.elseTarget.block == currentBlockIdx_)) {
                            reachable = true;
                        }
                    }
                }
            }

            // A body that can REACH its end returns `undefined` there, and a
            // pinned return is refused by the arm below when it does — so a
            // census entry for this function would be a manifest that does not
            // compile. Refused by the site table, which is the only way a
            // static fact reaches a dynamic instrument: reachability is a
            // property of the program, and no run can be asked about it.
            if (censusEnabled() && reachable && ilFn.returnType == il::Type::Dynamic &&
                !ilFn.name.empty() && !ilFn.isGenerator &&
                (ilFn.fnFlags & (BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC)) == 0) {
                addCensusSite("return " + manifestOwnerName(ilFn.name), il::CensusSite::Return,
                              /*refuses=*/true);
            }

            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            if (ilFn.returnType == il::Type::Void) {
                retInst.type = il::Type::Void;
            } else if (ilFn.returnType == il::Type::Dynamic ||
                       (!reachable && ilFn.returnType != il::Type::Str)) {
                Value retVal{il::kNoValue, il::Type::Void};
                if (ilFn.returnType == il::Type::Dynamic) {
                    il::ValueId undefVal = ilFn.valueCount++;
                    il::Instruction constInst;
                    constInst.op = il::Op::ConstUndefined;
                    constInst.type = il::Type::Dynamic;
                    constInst.result = undefVal;
                    emitInst(ilFn, constInst);
                    retVal = Value{undefVal, il::Type::Dynamic};
                } else {
                    il::ValueId dummyVal = ilFn.valueCount++;
                    il::Instruction constInst;
                    constInst.op = ilFn.returnType == il::Type::Bool ? il::Op::ConstBool
                                   : ilFn.returnType == il::Type::I32 ? il::Op::ConstI32
                                                                      : il::Op::ConstF64;
                    constInst.type = ilFn.returnType;
                    constInst.result = dummyVal;
                    emitInst(ilFn, constInst);
                    retVal = Value{dummyVal, ilFn.returnType};
                }
                retInst.type = retVal.type;
                retInst.operands = {retVal.id};
            } else {
                diags_.error(Span{}, "function " + ilFn.name +
                                         " can fall off the end but returns typed " +
                                         il::typeName(ilFn.returnType) +
                                         "; falling off yields undefined");
                return false;
            }
            emitInst(ilFn, retInst);
        }
    }

    if (functionEnvScope_ != SIZE_MAX) {
        envScopes_.pop_back();
        currentEnvValue_ = savedEnvValues_.back();
        savedEnvValues_.pop_back();
    }
    return true;
}

// The return annotation is deliberately not passed down: it is a hint, and the
// callers apply it — `lower()` and `lowerClosure` both check it against the
// proof BEFORE the body is lowered, because the IL return type is part of the
// calling convention.
bool Lowerer::lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
    return lowerFunctionBody(fnDecl.params, fnDecl.body, ilFn, fnDecl.isGenerator,
                             fnDecl.isAsync);
}

}  // namespace bronze::lower
