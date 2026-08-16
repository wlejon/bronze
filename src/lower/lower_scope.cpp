// Scopes, bindings and environment records: where a declaration lives, how a
// captured one is read and written, and how a closure value is produced over
// the environment innermost at its creation site.

#include <algorithm>
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
        auto& existing = varBindings_[it->second];
        if (existing.scopeDepth == currentScopeDepth_ && existing.isTdzHoisted && !isVar) {
            // The declaration that ends this binding's dead zone. Scope entry
            // already made the binding and gave it its environment slot; this
            // is the same binding reaching its initializer, not a second one.
            existing.isTdzHoisted = false;
            existing.type = type;
            existing.isConst = isConst;
            existing.isLet = isLet;
            existing.isInitialized = isInitialized;
            if (!existing.inEnv) existing.valueId = valId;
            return true;
        }
        if (isVar && existing.isVar) {
            if (valId != il::kNoValue && !existing.inEnv) {
                existing.valueId = valId;
                existing.type = type;
            }
            existing.isInitialized = existing.isInitialized || isInitialized;
            return true;
        }
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
    b.scopeDepth = isVar ? 0 : currentScopeDepth_;
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

il::ValueId Lowerer::currentEnv(il::Function& ilFn) {
    if (!generator_) return currentEnvValue_;
    il::ValueId here = generator_->frameEnv;
    for (size_t i = generator_->frameScope; i + 1 < envScopes_.size(); ++i) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::EnvGet;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {here};
        inst.envDepth = 0;
        inst.envIndex = envScopes_[i].childSlot;
        emitInst(ilFn, inst);
        here = res;
    }
    return here;
}

il::ValueId Lowerer::emitEnvCreate(uint32_t slotCount, il::Function& ilFn) {
    const il::ValueId enclosing = currentEnv(ilFn);
    il::ValueId parent = enclosing == il::kNoValue ? emitConstUndefined(ilFn) : enclosing;
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

bool Lowerer::envSlotIsLexical(uint32_t depth, uint32_t index) const {
    if (depth >= envScopes_.size()) return false;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    return index < scope.slotIsLexical.size() && scope.slotIsLexical[index];
}

bool Lowerer::envSlotIsImmutable(uint32_t depth, uint32_t index) const {
    if (depth >= envScopes_.size()) return false;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    return index < scope.slotIsImmutable.size() && scope.slotIsImmutable[index];
}

// Every read of an environment slot passes through here, so this is the one
// place 9.1.1.1.6's check has to be decided. A lexical slot gets the checked
// form UNCONDITIONALLY — not only where lowering can see a read above the
// declaration. The dead zone is a property of a moment in evaluation, and a
// closure over the slot can be called at any moment, so "lowering has already
// passed the declaration" is a fact about the source text and not about the
// run. Eliding on it is exactly the bug the case in
// `cases/temporal_dead_zone.js` pins.
Lowerer::Value Lowerer::emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn) {
    const bool lexical = envSlotIsLexical(depth, index);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = lexical ? il::Op::EnvGetTdz : il::Op::EnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {currentEnv(ilFn)};
    inst.envDepth = depth;
    inst.envIndex = index;
    if (lexical) {
        inst.keyIndex =
            getKeyConstantIndex(envScopes_[envScopes_.size() - 1 - depth].slotNames[index]);
    }
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

void Lowerer::openLexicalBindings(size_t scopeIndex,
                                  const std::vector<std::string>& lexicalNames,
                                  il::Function& ilFn) {
    const uint32_t depth = envDepthOf(scopeIndex);
    for (const auto& name : lexicalNames) {
        auto slotIt = envScopes_[scopeIndex].slotOf.find(name);
        // No slot means nothing can observe the binding uninitialized: no
        // closure reaches it and no read in its own scope is written above it,
        // so it lives in SSA and its declaration is the first mention of it.
        if (slotIt == envScopes_[scopeIndex].slotOf.end()) continue;
        // Something at this depth already owns the name: a parameter, or a
        // second declaration of it. Both are the declaration path's error to
        // report, and putting the marker in the slot would answer them with a
        // dead zone the language does not give either one.
        auto bound = activeVarMap_.find(name);
        if (bound != activeVarMap_.end() &&
            varBindings_[bound->second].scopeDepth == currentScopeDepth_) {
            continue;
        }
        const uint32_t slot = slotIt->second;
        envScopes_[scopeIndex].slotIsLexical[slot] = true;

        il::Instruction inst;
        inst.op = il::Op::EnvInitTdz;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {currentEnv(ilFn)};
        inst.envDepth = depth;
        inst.envIndex = slot;
        emitInst(ilFn, inst);

        if (!declareVariable(name, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/true,
                             /*isVar=*/false, /*isInitialized=*/false, il::kNoValue, Span{})) {
            continue;
        }
        varBindings_[activeVarMap_.at(name)].isTdzHoisted = true;
    }
}

// `assigning` says this write comes from an assignment rather than from the
// declaration that ends the binding's dead zone. 6.2.5.6 PutValue reaches
// 9.1.1.1.5 SetMutableBinding, which throws a ReferenceError for an
// uninitialized binding exactly as a read does — so an assignment to a lexical
// slot is CHECKED first, by the same instruction a read uses. Its result is
// discarded: what is wanted from it is the throw.
void Lowerer::emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn,
                         bool assigning) {
    // 9.1.1.1.5 step 4, the immutable arm: strict code throws and sloppy code
    // returns without storing. Both are DROPS of the store, which is why this
    // is decided here and not at each of the three call sites that assign to a
    // name (assignment, update, destructuring) — a store that reached the slot
    // from any of them would let a program rename its own function.
    if (assigning && envSlotIsImmutable(depth, index)) {
        if (strictCode_) emitImmutableAssign(envScopes_[envScopes_.size() - 1 - depth].slotNames[index], ilFn);
        return;
    }
    if (assigning && envSlotIsLexical(depth, index)) emitEnvGet(depth, index, ilFn);
    Value boxed = boxValueIfNeeded(val, ilFn);
    il::Instruction inst;
    inst.op = il::Op::EnvSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {currentEnv(ilFn), boxed.id};
    inst.envDepth = depth;
    inst.envIndex = index;
    emitInst(ilFn, inst);
}

// The strict half of the rule above, as an instruction. Its result is
// discarded: what is wanted from it is the throw, and the backend's exception
// test after it is what carries control to the handler.
void Lowerer::emitImmutableAssign(const std::string& name, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ImmutableAssign;
    inst.type = il::Type::Dynamic;
    inst.result = ilFn.valueCount++;
    inst.keyIndex = getKeyConstantIndex(name);
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
        emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, val, ilFn, /*assigning=*/true);
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
                         const std::vector<std::string>& extraDeclarations,
                         const std::vector<std::string>& extraLexicalDeclarations) {
    currentScopeDepth_++;
    std::vector<std::string> slots;
    auto addSlot = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(slots.begin(), slots.end(), name) != slots.end()) return;
        slots.push_back(name);
    };
    // for-of's loop variable is declared by the loop HEAD but belongs to the
    // body's scope, so it is not in the statement list and has to be named
    // here. Without it a closure over the loop variable would find no slot and
    // read SSA that the next iteration overwrites. A destructuring head names
    // several.
    for (const auto& name : extraDeclarations) {
        if (memoryNames_.contains(name)) addSlot(name);
    }
    // Unfiltered, unlike everything else here: a switch body's lexical binding
    // needs a slot BECAUSE the dead zone is what makes it well defined, not
    // because something captured it.
    for (const auto& name : extraLexicalDeclarations) addSlot(name);
    for (const auto& name : ast::getScopeDeclarations(stmts)) {
        if (memoryNames_.contains(name)) addSlot(name);
    }
    if (slots.empty()) {
        scopeHasEnv_.push_back(false);
        return;
    }
    EnvScopeInfo info;
    // One more slot, in a generator only, for the record of whatever scope opens
    // inside this one. The chain of them is the only way down from the frame,
    // and a suspension needs one — see `currentEnv`. Named, so a dump of the
    // record says what the extra word is.
    if (generator_) {
        info.childSlot = static_cast<uint32_t>(slots.size());
        slots.emplace_back(generatorEnvSlotName());
    }
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.slotNames = slots;
    info.slotIsLexical.assign(slots.size(), false);
    info.slotIsImmutable.assign(slots.size(), false);
    const il::ValueId parentRecord = generator_ ? currentEnv(ilFn) : il::kNoValue;
    const uint32_t parentChildSlot =
        generator_ ? envScopes_.back().childSlot : UINT32_MAX;
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    if (generator_) {
        il::Instruction link;
        link.op = il::Op::EnvSet;
        link.type = il::Type::Void;
        link.result = il::kNoValue;
        link.operands = {parentRecord, info.envValue};
        link.envDepth = 0;
        link.envIndex = parentChildSlot;
        emitInst(ilFn, link);
    }
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    scopeHasEnv_.push_back(true);

    std::vector<std::string> lexical = extraLexicalDeclarations;
    for (auto& name : ast::getLexicalDeclarations(stmts)) lexical.push_back(std::move(name));
    openLexicalBindings(envScopes_.size() - 1, lexical, ilFn);
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
    // A function written inside a generator body is not one: its `return` is an
    // ordinary return and it has no frame of the enclosing machine's.
    auto outerGenerator = std::move(generator_);
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
    if (const auto* fnExpr = dynamic_cast<const ast::FunctionExpr*>(&site)) {
        strictCode_ = fnExpr->strict;
    } else if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(&site)) {
        strictCode_ = fnDecl->strict;
    }
    auto outerEnvBase = functionEnvBase_;
    auto outerEnvScope = functionEnvScope_;
    // `var` names are per FUNCTION, so the nested body's list must not outlive
    // it: leaving the callee's behind would make an enclosing free name look
    // like a `var` the callee declared.
    auto outerVarNames = functionVarNames_;
    size_t outerEnvDepth = envScopes_.size();

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
            // The whole point of the record: 15.2.5 step 3 is
            // CreateImmutableBinding, so an inner `fact = x` stores nothing.
            info.slotIsImmutable.assign(1, true);
            info.envValue = nfeEnv;
            envScopes_.push_back(std::move(info));
            currentEnvValue_ = nfeEnv;
        }
    }

    // The body is lowered into a DIFFERENT function, so every piece of state
    // saved above now describes the nested one — and that has to be undone on
    // the failure path as well. Returning early from here once left the
    // enclosing lowering holding the callee's binding map, its (cleared) jump
    // stack and its (cleared) label stack, so a caller that unwound past a
    // labelled statement popped an empty vector, and one that went on to
    // report a second diagnostic resolved names against the wrong scope. The
    // caller stops at the first error either way, so this restores and then
    // reports rather than reporting and leaving the wreckage.
    const auto* siteFnExpr = dynamic_cast<const ast::FunctionExpr*>(&site);
    const auto* siteFnDecl = dynamic_cast<const ast::FunctionDecl*>(&site);
    const bool isGenerator = (siteFnExpr && siteFnExpr->isGenerator) ||
                             (siteFnDecl && siteFnDecl->isGenerator);
    const bool isAsync =
        (siteFnExpr && siteFnExpr->isAsync) || (siteFnDecl && siteFnDecl->isAsync);
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
