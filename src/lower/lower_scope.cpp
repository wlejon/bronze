// Scopes, bindings and environment records: where a declaration lives, how
// a captured one is read and written, and how a closure value is produced
// over the environment innermost at its creation site (docs/0007).

#include <string>
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

// --- environment emission (docs/0007) --------------------------------
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
                         const std::string& extraDeclaration) {
    currentScopeDepth_++;
    std::vector<std::string> slots;
    // for-of's loop variable is declared by the loop HEAD but belongs to
    // the body's scope, so it is not in the statement list and has to be
    // named here. Without it a closure over the loop variable would find no
    // slot and read SSA that the next iteration overwrites (docs/0011
    // decision 5).
    if (!extraDeclaration.empty() && capturedNames_.contains(extraDeclaration)) {
        slots.push_back(extraDeclaration);
    }
    for (const auto& name : ast::getScopeDeclarations(stmts)) {
        if (capturedNames_.contains(name)) slots.push_back(name);
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
    std::vector<std::string> toRemove;
    for (const auto& entry : activeVarMap_) {
        if (varBindings_[entry.second].scopeDepth == currentScopeDepth_ && !varBindings_[entry.second].isVar) {
            toRemove.push_back(entry.first);
        }
    }
    for (const auto& name : toRemove) {
        activeVarMap_.erase(name);
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

// Shared by function expressions and nested function declarations:
// both produce a closure value over the environment that is innermost
// at the creation site (docs/0007 decision 4).
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
    // Every function expression is a closure: it gets the synthetic
    // environment parameter whether or not it turns out to capture
    // anything (docs/0007). An unused one costs a parameter.
    newFn.needsEnv = true;
    newFn.params.push_back({"__env", il::Type::Dynamic});
    // An arrow deliberately does NOT take a receiver parameter, however it
    // is called: its `this` is the enclosing function's, read from the
    // environment (docs/0012 decision 3). Giving it one would be a second,
    // contradictory answer to the same question.
    if (!isArrow && ast::usesThis(body)) {
        newFn.needsThis = true;
        newFn.params.push_back({"__this", il::Type::Dynamic});
    }
    // A closure's parameters and return are always the uniform dynamic
    // convention, and an annotation cannot change that (docs/0010 decision
    // 6).
    //
    // A closure PARAMETER has no proof and cannot have one: a signature is
    // inferred by joining over every call site, which is sound only for a
    // name whose callers this compilation can enumerate, and a closure is
    // reached through a function value — decision 5 excludes it by
    // construction. So the proof handed to the check is `dynamic`, which is
    // the honest report of "nothing was observed here", and every parameter
    // annotation on a closure is discarded with a warning saying so.
    for (const auto& param : params) {
        newFn.params.push_back({param.name, il::Type::Dynamic});
        if (!checkAnnotation(param.typeAnnotation, span, param.name, types::Type::dynamic())) {
            return std::nullopt;
        }
    }
    // The RETURN is different in kind, and this is the closure proof surface
    // docs/0010 recorded as missing: what a body returns is a fact about the
    // body alone, and inference already joins every `return` in it. So a
    // return annotation on a closure can agree with a proof after all, and a
    // visibly correct one no longer reports as unprovable. It still buys
    // nothing — the IL return type above stays dynamic, because that is the
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
    auto outerLoopStack = loopStack_;
    auto outerScopeHasEnv = scopeHasEnv_;
    auto outerCaptured = capturedNames_;
    auto outerEnvValue = currentEnvValue_;
    auto outerThisValue = currentThisValue_;
    auto outerIsArrow = currentFunctionIsArrow_;
    currentFunctionIsArrow_ = isArrow;
    auto outerEnvBase = functionEnvBase_;
    auto outerEnvScope = functionEnvScope_;
    size_t outerEnvDepth = envScopes_.size();

    if (!lowerFunctionBody(params, body, newFn)) {
        return std::nullopt;
    }

    varBindings_ = outerVarBindings;
    activeVarMap_ = outerActiveVarMap;
    currentScopeDepth_ = outerScopeDepth;
    varDeclCounter_ = outerVarDeclCounter;
    loopStack_ = outerLoopStack;
    currentBlockIdx_ = outerBlockIdx;
    scopeHasEnv_ = outerScopeHasEnv;
    capturedNames_ = outerCaptured;
    currentEnvValue_ = outerEnvValue;
    currentThisValue_ = outerThisValue;
    currentFunctionIsArrow_ = outerIsArrow;
    functionEnvBase_ = outerEnvBase;
    functionEnvScope_ = outerEnvScope;
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
    inst.immI32 = static_cast<int32_t>(params.size());
    inst.operands = {envArg};
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
