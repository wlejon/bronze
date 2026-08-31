// The CLOSURE VALUE: the IL function a function expression or a nested function
// declaration becomes, the per-function state saved and restored around its
// body, and the `func.create` that captures the record innermost at the site.

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// Shared by function expressions and nested function declarations: both produce
// a closure value over the environment that is innermost at the creation site.
std::optional<Lowerer::Value> Lowerer::lowerNamedEvaluation(const ast::Expr& expr,
                                                            const std::string& name,
                                                            il::Function& ilFn) {
    const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&expr);
    // A function expression that wrote its OWN name keeps it: 15.2.5 binds that
    // name inside the body and 8.6.2 does not apply, so `const f = function g()
    // {}` has `f.name === "g"`.
    if (!fn || !fn->name.empty()) return lowerExpr(expr, ilFn);
    return lowerClosure(*fn, /*declaredName=*/"", name, fn->params, fn->returnType, fn->body,
                        fn->span, ilFn, fn->isArrow);
}

std::optional<Lowerer::Value> Lowerer::lowerClosure(const ast::Node& site,
                                                    const std::string& declaredName,
                                                    const std::optional<std::string>& jsName,
                                                    const std::vector<ast::Param>& params,
                                                    const std::string& returnTypeAnn,
                                                    const std::vector<ast::StmtPtr>& body,
                                                    Span span, il::Function& ilFn, bool isArrow,
                                                    bool bindsOwnName) {
    std::string fnName = declaredName;
    if (fnName.empty()) {
        fnName = "__anon_fn_" + std::to_string(ilModule_.functions.size());
    }
    il::Function newFn;
    newFn.name = fnName;
    // The key constant is allocated here even for the empty name, because "" is
    // a real answer — 10.2.9 gives an anonymous function expression exactly that
    // — and the runtime has to be able to tell it from "no name recorded".
    if (jsName) newFn.nameKeyIndex = getKeyConstantIndex(*jsName);
    newFn.returnType = il::Type::Dynamic;
    const size_t outerEnvDepth = envScopes_.size();
    const auto* siteFnExpr = dynamic_cast<const ast::FunctionExpr*>(&site);
    const auto* siteFnDecl = dynamic_cast<const ast::FunctionDecl*>(&site);
    const bool isGenerator = (siteFnExpr && siteFnExpr->isGenerator) ||
                             (siteFnDecl && siteFnDecl->isGenerator);
    const bool isAsync =
        (siteFnExpr && siteFnExpr->isAsync) || (siteFnDecl && siteFnDecl->isAsync);

    // 15.2.5 InstantiateOrdinaryFunctionExpression, the named branch: a
    // function expression that wrote its own name is created in a declarative
    // environment of its own holding ONE immutable binding of that name, and
    // that record — not the enclosing one — is the closure's scope. So the
    // recursive reference in `(function fact(n) { return n * fact(n - 1) })` is
    // an ordinary capture: the body resolves `fact` through the environment
    // chain like any other free name, one hop further out than its own record.
    //
    // Built only where the body (or a parameter default, which is code of this
    // function that the body does not contain) actually mentions the name. The
    // binding is unobservable otherwise, and paying a record per evaluation for
    // it would change the IL of every `function f() {}` in the program.
    il::ValueId nfeEnv = il::kNoValue;
    if (bindsOwnName && !isArrow && !declaredName.empty()) {
        auto referenced = ast::getReferencedNames(body);
        for (auto& name : ast::getParamReferencedNames(params)) {
            referenced.insert(std::move(name));
        }
        if (referenced.contains(declaredName)) {
            nfeEnv = emitEnvCreate(1, ilFn);
            EnvScopeInfo info;
            info.slotOf[declaredName] = 0;
            info.slotNames = {declaredName};
            info.slotIsLexical.assign(1, false);
            info.slotIsDefiniteInit.assign(1, false);
            // The whole point of the record: 15.2.5 step 3 is
            // CreateImmutableBinding, so an inner `fact = x` stores nothing.
            info.slotImmutable.assign(1, SlotImmutability::Silent);
            info.envValue = nfeEnv;
            envScopes_.push_back(std::move(info));
            currentEnvValue_ = nfeEnv;
        }
    }

    // A function only requires an environment parameter when it captures from an
    // enclosing non-module scope, is a generator/async function, has an NFE
    // binding, or is an arrow reading outer `this`/`arguments`. Top-level methods
    // and closures without outer lexical scopes have `needsEnv == false`, allowing
    // direct calls and IC latching.
    const size_t nonModuleScopeThreshold =
        (moduleEnvScope_ != std::numeric_limits<size_t>::max()) ? 1 : 0;
    bool needsEnv = true;
    if (!isGenerator && !isAsync && nfeEnv == il::kNoValue &&
        (!isArrow || (!ast::usesThis(params, body) && !ast::usesArguments(params, body))) &&
        outerEnvDepth <= nonModuleScopeThreshold) {
        needsEnv = false;
    }

    newFn.needsEnv = needsEnv;
    if (needsEnv) {
        newFn.params.push_back({"__env", il::Type::Dynamic});
    }
    // An arrow deliberately does NOT take a receiver parameter, however it is
    // called: its `this` is the enclosing function's, read from the
    // environment. Giving it one would be a second, contradictory answer to the
    // same question.
    if (!isArrow && ast::usesThis(params, body)) {
        newFn.needsThis = true;
        newFn.params.push_back({"__this", il::Type::Dynamic});
    }
    // An arrow has no `arguments` either, and for the same reason: it sees the
    // enclosing function's through the environment.
    if (!isArrow && ast::usesArguments(params, body)) {
        newFn.needsArguments = true;
        newFn.params.push_back({"__arguments", il::Type::Dynamic});
    }
    // A closure's parameters and return are always the uniform dynamic
    // convention, and an annotation cannot change that.
    //
    // A closure PARAMETER has no proof and cannot have one: a signature is
    // inferred by joining over every call site, which is sound only for a name
    // whose callers this compilation can enumerate, and a closure is reached
    // through a function value — signature specialization excludes it by
    // construction. So the proof handed to the check is `dynamic`, which is the
    // honest report of "nothing was observed here", and every parameter
    // annotation on a closure is discarded with a warning saying so.
    for (const auto& param : params) {
        newFn.params.push_back(
            {param.pattern ? "__pattern" + std::to_string(newFn.params.size()) : param.name,
             il::Type::Dynamic});
        if (!checkAnnotation(param.typeAnnotation, span, param.name, types::Type::dynamic())) {
            return std::nullopt;
        }
    }
    applyParamShape(params, newFn);
    // The RETURN is different in kind: what a body returns is a fact about
    // the body alone, and inference joins every `return` in it — so a return
    // annotation on a closure CAN agree with a proof, where a parameter
    // annotation cannot (that needs escape analysis). It buys nothing at the
    // IL level: the return type above stays dynamic, because that is the
    // calling convention and no annotation may widen it.
    //
    // Reported on `fnName`, which for an anonymous function expression is
    // the synthesized `__anon_fn_N` — deliberately, because that is the name
    // it has in the IL dump and the span already points at the source.
    if (!checkAnnotation(returnTypeAnn, span, fnName, provenClosureReturn(site))) {
        return std::nullopt;
    }
    newFn.valueCount = static_cast<uint32_t>(newFn.params.size());
    // `span` is the SITE's, and for a class that is the whole `class C {...}`
    // rather than the constructor's own text — which is exactly what
    // `Class.toString()` has to return, so no caller special-cases it.
    newFn.sourceFile = span.file;
    newFn.sourceBegin = span.begin;
    newFn.sourceEnd = span.end;

    size_t outerBlockIdx = currentBlockIdx_;
    auto outerVarBindings = varBindings_;
    auto outerActiveVarMap = activeVarMap_;
    // The `func.ref` memo is per IL FUNCTION: a `Value` in it names an
    // instruction result, and result ids are numbered within one function.
    // `lowerFunctionBody` clears it on the way in, which is only half of what
    // an entry/exit pair needs — without the restore, the enclosing function
    // came back holding the CALLEE's ids, so a later mention of a top-level
    // `function` in the caller read whatever instruction happened to have that
    // number. `(function () { h(g, 4); })(); h(g, 3);` called the anonymous
    // function twice and never called `h` again. Saved with the rest of the
    // per-function state, and restored below with it.
    auto outerFunctionRefMap = functionRefMap_;
    auto outerScopeDepth = currentScopeDepth_;
    auto outerVarDeclCounter = varDeclCounter_;
    auto outerJumpStack = jumpStack_;
    // Labels do not cross a function boundary: `break outer` inside a nested
    // function names nothing, and the outer label must not be visible to it.
    auto outerLabelStack = labelStack_;
    labelStack_.clear();
    auto outerScopeHasEnv = scopeHasEnv_;
    auto outerCaptured = capturedNames_;
    auto outerMemoryNames = memoryNames_;
    // A `return` inside a nested function runs THAT function's finallys and
    // none of the enclosing ones, exactly as `break outer` names nothing
    // across the same boundary.
    auto outerCleanupStack = cleanupStack_;
    cleanupStack_.clear();
    // A function written inside a generator body is not one: its `return` is an
    // ordinary return and it has no frame of the enclosing machine's.
    // Moved field by field behind an explicit engagement test rather than as
    // one `std::optional` move: gcc's uninitialised-use analysis cannot see
    // that a disengaged optional's payload is never read by the move, and
    // reports every field of the context as possibly uninitialised.
    std::optional<GeneratorContext> outerGenerator;
    if (generator_.has_value()) outerGenerator.emplace(std::move(*generator_));
    generator_.reset();
    auto outerHandler = currentHandler_;
    currentHandler_ = il::kNoBlock;
    auto outerEnvValue = currentEnvValue_;
    auto outerThisValue = currentThisValue_;
    auto outerIsArrow = currentFunctionIsArrow_;
    currentFunctionIsArrow_ = isArrow;
    // Strictness comes from the FUNCTION NODE, not from the enclosing code:
    // the parser has already resolved inheritance (a function inside strict
    // code is strict) and a body's own `"use strict"` (a strict function
    // inside sloppy code). `site` is that node, which is why nothing here has
    // to be passed a flag.
    const bool outerStrict = strictCode_;
    if (siteFnExpr) {
        strictCode_ = siteFnExpr->strict;
    } else if (siteFnDecl) {
        strictCode_ = siteFnDecl->strict;
    }
    auto outerEnvBase = functionEnvBase_;
    auto outerEnvScope = functionEnvScope_;
    // `var` names are per FUNCTION, so the nested body's list must not outlive
    // it: leaving the callee's behind would make an enclosing free name look
    // like a `var` the callee declared.
    auto outerVarNames = functionVarNames_;
    // The typed-element binding scan asks about the body being lowered NOW;
    // lowerFunctionBody points it at the nested body, so the outer pointer
    // comes back with everything else here.
    const auto* outerBodyStmts = currentBodyStmts_;

    // A `FunctionDecl` site is always the ordinary form — there is no syntax
    // for a declaration that is a method or an arrow — so only the expression
    // carries a kind worth reading.
    newFn.fnFlags = functionObjectFlags(
        siteFnExpr ? siteFnExpr->kind : ast::FunctionKind::Normal, isGenerator, isAsync);
    // The `--pins` signature entries, before the body is lowered: the parameter
    // types are what the body's reads of them resolve against, and the return
    // type is part of the calling convention a recursive call already reads.
    if (!applySignaturePins(params, span, newFn)) return std::nullopt;
    // And the proof, which since stage E4 covers the case a pin used to be the
    // only answer for: a nested declaration whose every call site this
    // compilation can enumerate (`planClosureParamNumbers`). After the pins, so
    // that the manifest's error reporting still owns a position it cannot
    // honour; both only ever move a slot from Dynamic to F64, so the order
    // decides nothing else.
    applyProvenClosureParams(site, params, newFn);
    const bool bodyOk = lowerFunctionBody(params, body, newFn, isGenerator, isAsync);
    // The name's record is visible to the BODY and to nothing else — 15.2.5
    // creates it around the closure, not in the scope that wrote the
    // expression — so it leaves the stack the moment the body is lowered, and
    // before the balance check below counts what is left.
    if (nfeEnv != il::kNoValue) envScopes_.pop_back();

    generator_ = std::move(outerGenerator);
    strictCode_ = outerStrict;
    varBindings_ = outerVarBindings;
    activeVarMap_ = outerActiveVarMap;
    functionRefMap_ = outerFunctionRefMap;
    currentScopeDepth_ = outerScopeDepth;
    varDeclCounter_ = outerVarDeclCounter;
    jumpStack_ = outerJumpStack;
    labelStack_ = outerLabelStack;
    currentBlockIdx_ = outerBlockIdx;
    scopeHasEnv_ = outerScopeHasEnv;
    capturedNames_ = outerCaptured;
    memoryNames_ = outerMemoryNames;
    cleanupStack_ = outerCleanupStack;
    currentHandler_ = outerHandler;
    currentEnvValue_ = outerEnvValue;
    currentThisValue_ = outerThisValue;
    currentFunctionIsArrow_ = outerIsArrow;
    functionEnvBase_ = outerEnvBase;
    functionEnvScope_ = outerEnvScope;
    functionVarNames_ = outerVarNames;
    currentBodyStmts_ = outerBodyStmts;
    if (!bodyOk) {
        if (envScopes_.size() > outerEnvDepth) envScopes_.resize(outerEnvDepth);
        return std::nullopt;
    }
    if (envScopes_.size() != outerEnvDepth) {
        diags_.error(span, "internal: environment stack unbalanced after lowering " + fnName);
        return std::nullopt;
    }

    uint32_t createdFnIdx = static_cast<uint32_t>(ilModule_.functions.size());
    // NOT registered in `functionIndices_`. That map is the module's symbol
    // table — the names a reference anywhere in the module may resolve to
    // directly — and a nested function's name is not one of them: it is a
    // binding of the scope that wrote it, reached through `activeVarMap_` or
    // the environment chain like any other. Registering it put a scope-local
    // name into a module-wide table, and the two ways that was wrong are the
    // reason this line is a comment: an unrelated function's `f()` resolved to
    // a closure it cannot see, and a nested `function f` OVERWROTE a top-level
    // `f` of the same name, so every later call to the top-level one was
    // redirected to the inner one.
    ilModule_.functions.push_back(std::move(newFn));
    // Valid until the next closure is lowered, and read by exactly one caller:
    // `lowerClass`, immediately after this returns, to record which module
    // function a method NAME denotes (direct_method_table.h). A return
    // value would have been better and is not available — every caller of
    // `lowerClosure` wants the closure VALUE, and there is one of these.
    lastClosureFnIndex_ = createdFnIdx;

    // The closure captures the environment that is innermost right
    // here, at its creation site — or, for a named function expression, the
    // one-slot record built above, which chains to it. `currentEnv` cannot
    // answer that: the record left the scope stack with the body, because
    // nothing outside the closure may resolve a name through it.
    const il::ValueId enclosingEnv = nfeEnv != il::kNoValue ? nfeEnv : currentEnv(ilFn);
    il::ValueId envArg =
        enclosingEnv == il::kNoValue ? emitConstUndefined(ilFn) : enclosingEnv;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateFunction;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.calleeIndex = createdFnIdx;
    // The arity a CALL adapts to. A rest parameter is not one of them: it is
    // built from whatever is left over, so counting it would have short calls
    // padded with an `undefined` that the rest array then contained.
    // Zero for a closure that owns an `arguments` object, for the reason
    // `il::Function::adaptArity` records: padding would erase the difference
    // between `f(1)` and `f(1, undefined)`, which `arguments.length` sees.
    inst.immI32 = ilModule_.functions[createdFnIdx].needsArguments
                      ? 0
                      : static_cast<int32_t>(params.size()) -
                            (params.empty() || !params.back().isRest ? 0 : 1);
    inst.operands = {envArg};
    emitInst(ilFn, inst);
    // 15.2.5 step 5, InitializeBinding: the record holds the closure it is the
    // scope of, which is what closes the cycle the recursion walks. Written
    // through the record VALUE rather than through `emitEnvSet`, because the
    // scope that owns the slot is no longer on the stack a (depth, index) pair
    // is counted against.
    if (nfeEnv != il::kNoValue) {
        il::Instruction bind;
        bind.op = il::Op::EnvSet;
        bind.type = il::Type::Void;
        bind.result = il::kNoValue;
        bind.operands = {nfeEnv, res};
        bind.envDepth = 0;
        bind.envIndex = 0;
        emitInst(ilFn, bind);
    }
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
