// Scopes, bindings and environment records: where a declaration lives, how a
// captured one is read and written, and how a closure value is produced over
// the environment innermost at its creation site.

#include <string>
#include <utility>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet,
                              bool isVar, bool isInitialized, il::ValueId valId, Span span) {
    auto it = activeVarMap_.find(name);
    if (it != activeVarMap_.end()) {
        const auto& existing = varBindings_[it->second];
        if (existing.scopeDepth == currentScopeDepth_ && !isVar) {
            diags_.error(span, "redeclaration of variable '" + name + "' in same scope");
            return false;
        }
    }
    VarBinding b;
    if (it != activeVarMap_.end()) b.shadowedBinding = it->second;
    b.name = name;
    b.type = type;
    b.isConst = isConst;
    b.isLet = isLet;
    b.isVar = isVar;
    b.isInitialized = isInitialized;
    b.declOrder = varDeclCounter_++;
    b.scopeDepth = currentScopeDepth_;
    b.valueId = valId;

    // A captured declaration lives in its scope's environment. `var` is
    // function-scoped wherever it is written, so it belongs to the
    // function's environment, not the innermost block's.
    size_t ownerScope = SIZE_MAX;
    if (isVar) {
        if (functionEnvScope_ != SIZE_MAX &&
            envScopes_[functionEnvScope_].slotOf.contains(name)) {
            ownerScope = functionEnvScope_;
        }
    } else if (envScopes_.size() > functionEnvBase_ &&
               envScopes_.back().slotOf.contains(name)) {
        ownerScope = envScopes_.size() - 1;
    }
    if (ownerScope != SIZE_MAX) {
        b.inEnv = true;
        b.envScopeIndex = ownerScope;
        b.envSlot = envScopes_[ownerScope].slotOf.at(name);
        b.valueId = il::kNoValue;
    }

    size_t idx = varBindings_.size();
    varBindings_.push_back(b);
    activeVarMap_[name] = idx;
    return true;
}

// --- environment emission --------------------------------
il::ValueId Lowerer::emitConstUndefined(il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ConstUndefined;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);
    return res;
}

il::ValueId Lowerer::emitEnvCreate(uint32_t slotCount, il::Function& ilFn) {
    il::ValueId parent =
        currentEnvValue_ == il::kNoValue ? emitConstUndefined(ilFn) : currentEnvValue_;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::EnvCreate;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {parent};
    inst.immI32 = static_cast<int32_t>(slotCount);
    emitInst(ilFn, inst);
    return res;
}

// The module scope's record, reached through the runtime rather than through a
// parameter. `main` publishes it once; anything that needs it loads it.
il::ValueId Lowerer::emitModuleEnvGet(il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ModuleEnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);
    return res;
}

void Lowerer::emitModuleEnvSet(il::ValueId env, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ModuleEnvSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {env};
    emitInst(ilFn, inst);
}

Lowerer::Value Lowerer::emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::EnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {currentEnvValue_};
    inst.envDepth = depth;
    inst.envIndex = index;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

void Lowerer::emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(val, ilFn);
    il::Instruction inst;
    inst.op = il::Op::EnvSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {currentEnvValue_, boxed.id};
    inst.envDepth = depth;
    inst.envIndex = index;
    emitInst(ilFn, inst);
}

uint32_t Lowerer::envDepthOf(size_t scopeIndex) const {
    return static_cast<uint32_t>(envScopes_.size() - 1 - scopeIndex);
}

Lowerer::Value Lowerer::readBinding(const VarBinding& b, il::Function& ilFn) {
    if (!b.inEnv) return Value{b.valueId, b.type};
    return emitEnvGet(envDepthOf(b.envScopeIndex), b.envSlot, ilFn);
}

void Lowerer::writeBinding(VarBinding& b, Value val, il::Function& ilFn) {
    if (b.inEnv) {
        emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, val, ilFn);
        b.isInitialized = true;
        return;
    }
    b.valueId = val.id;
    b.type = val.type;
    b.isInitialized = true;
}

// A free variable of the function being lowered, resolved against the
// environments of enclosing scopes.
bool Lowerer::findEnclosingEnvVar(const std::string& name, uint32_t& depth,
                                  uint32_t& index) const {
    for (size_t i = envScopes_.size(); i-- > 0;) {
        auto it = envScopes_[i].slotOf.find(name);
        if (it != envScopes_[i].slotOf.end()) {
            depth = envDepthOf(i);
            index = it->second;
            return true;
        }
    }
    return false;
}

void Lowerer::enterScope() {
    currentScopeDepth_++;
    scopeHasEnv_.push_back(false);
}

// Scope entry that first gives the scope an environment record if any
// of its own declarations are captured. For a loop body this runs once
// per iteration at runtime, which is exactly the per-iteration binding
// the language specifies.
void Lowerer::enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn,
                         const std::vector<std::string>& extraDeclarations) {
    currentScopeDepth_++;
    std::vector<std::string> slots;
    // for-of's loop variable is declared by the loop HEAD but belongs to the
    // body's scope, so it is not in the statement list and has to be named
    // here. Without it a closure over the loop variable would find no slot and
    // read SSA that the next iteration overwrites. A destructuring head names
    // several.
    for (const auto& name : extraDeclarations) {
        if (!name.empty() && memoryNames_.contains(name)) slots.push_back(name);
    }
    for (const auto& name : ast::getScopeDeclarations(stmts)) {
        if (memoryNames_.contains(name)) slots.push_back(name);
    }
    if (slots.empty()) {
        scopeHasEnv_.push_back(false);
        return;
    }
    EnvScopeInfo info;
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    scopeHasEnv_.push_back(true);
}

void Lowerer::exitScope() {
    // Leaving a scope UNCOVERS what its declarations shadowed; it does not
    // delete the name. Erasing outright made an inner `let x` remove the
    // enclosing `x` for the rest of the function.
    std::vector<std::pair<std::string, size_t>> toRestore;
    for (const auto& entry : activeVarMap_) {
        const auto& binding = varBindings_[entry.second];
        if (binding.scopeDepth == currentScopeDepth_ && !binding.isVar) {
            toRestore.emplace_back(entry.first, binding.shadowedBinding);
        }
    }
    for (const auto& [name, shadowed] : toRestore) {
        if (shadowed == SIZE_MAX) {
            activeVarMap_.erase(name);
        } else {
            activeVarMap_[name] = shadowed;
        }
    }
    if (!scopeHasEnv_.empty()) {
        if (scopeHasEnv_.back()) {
            envScopes_.pop_back();
            currentEnvValue_ = savedEnvValues_.back();
            savedEnvValues_.pop_back();
        }
        scopeHasEnv_.pop_back();
    }
    currentScopeDepth_--;
}

// Shared by function expressions and nested function declarations: both produce
// a closure value over the environment that is innermost at the creation site.
std::optional<Lowerer::Value> Lowerer::lowerClosure(const ast::Node& site,
                                                    const std::string& declaredName,
                                                    const std::vector<ast::Param>& params,
                                                    const std::string& returnTypeAnn,
                                                    const std::vector<ast::StmtPtr>& body,
                                                    Span span, il::Function& ilFn, bool isArrow) {
    std::string fnName = declaredName;
    if (fnName.empty()) {
        fnName = "__anon_fn_" + std::to_string(ilModule_.functions.size());
    }
    il::Function newFn;
    newFn.name = fnName;
    newFn.returnType = il::Type::Dynamic;
    // Every function expression is a closure: it gets the synthetic environment
    // parameter whether or not it turns out to capture anything. An unused one
    // costs a parameter.
    newFn.needsEnv = true;
    newFn.params.push_back({"__env", il::Type::Dynamic});
    // An arrow deliberately does NOT take a receiver parameter, however it is
    // called: its `this` is the enclosing function's, read from the
    // environment. Giving it one would be a second, contradictory answer to the
    // same question.
    if (!isArrow && ast::usesThis(body)) {
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

    size_t outerBlockIdx = currentBlockIdx_;
    auto outerVarBindings = varBindings_;
    auto outerActiveVarMap = activeVarMap_;
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
    auto outerHandler = currentHandler_;
    currentHandler_ = il::kNoBlock;
    auto outerEnvValue = currentEnvValue_;
    auto outerThisValue = currentThisValue_;
    auto outerIsArrow = currentFunctionIsArrow_;
    currentFunctionIsArrow_ = isArrow;
    auto outerEnvBase = functionEnvBase_;
    auto outerEnvScope = functionEnvScope_;
    // `var` names are per FUNCTION, so the nested body's list must not outlive
    // it: leaving the callee's behind would make an enclosing free name look
    // like a `var` the callee declared.
    auto outerVarNames = functionVarNames_;
    size_t outerEnvDepth = envScopes_.size();

    // The body is lowered into a DIFFERENT function, so every piece of state
    // saved above now describes the nested one — and that has to be undone on
    // the failure path as well. Returning early from here once left the
    // enclosing lowering holding the callee's binding map, its (cleared) jump
    // stack and its (cleared) label stack, so a caller that unwound past a
    // labelled statement popped an empty vector, and one that went on to
    // report a second diagnostic resolved names against the wrong scope. The
    // caller stops at the first error either way, so this restores and then
    // reports rather than reporting and leaving the wreckage.
    // A named function EXPRESSION only: a nested function declaration's name
    // is a binding of the enclosing scope and resolves through it, so it must
    // not be caught by the limitation this records (see emitReferenceError).
    const bool isNamedFunctionExpr =
        !isArrow && !declaredName.empty() && dynamic_cast<const ast::FunctionExpr*>(&site);
    if (isNamedFunctionExpr) namedFunctionExprs_.push_back(declaredName);
    const bool bodyOk = lowerFunctionBody(params, body, newFn);
    if (isNamedFunctionExpr) namedFunctionExprs_.pop_back();

    varBindings_ = outerVarBindings;
    activeVarMap_ = outerActiveVarMap;
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
    if (!bodyOk) {
        if (envScopes_.size() > outerEnvDepth) envScopes_.resize(outerEnvDepth);
        return std::nullopt;
    }
    if (envScopes_.size() != outerEnvDepth) {
        diags_.error(span, "internal: environment stack unbalanced after lowering " + fnName);
        return std::nullopt;
    }

    uint32_t createdFnIdx = static_cast<uint32_t>(ilModule_.functions.size());
    functionIndices_[fnName] = createdFnIdx;
    ilModule_.functions.push_back(std::move(newFn));

    // The closure captures the environment that is innermost right
    // here, at its creation site.
    il::ValueId envArg =
        currentEnvValue_ == il::kNoValue ? emitConstUndefined(ilFn) : currentEnvValue_;
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
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
