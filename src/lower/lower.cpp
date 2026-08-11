#include "lower/lower.h"

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<il::Module> Lowerer::lower() {
    ilModule_.name = astModule_.name;

    std::vector<const ast::Stmt*> topLevelStmts;
    for (const auto& stmtPtr : astModule_.body) {
        if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmtPtr.get())) {
            if (functionIndices_.contains(fnDecl->name)) {
                diags_.error(fnDecl->span, "duplicate function name: " + fnDecl->name);
                return std::nullopt;
            }
            il::Function fn;
            fn.name = fnDecl->name;
            fn.isExported = fnDecl->isExported;
            // A body that mentions `this` gets the synthetic receiver
            // parameter ahead of its source parameters; every call
            // site supplies it, direct ones included, so this costs
            // nothing to functions that do not use it (docs/0008).
            if (ast::usesThis(fnDecl->body)) {
                fn.needsThis = true;
                fn.params.push_back({"__this", il::Type::Dynamic});
            }
            // Every source parameter starts on the uniform dynamic
            // convention and stays there unless a PROOF moves it. An
            // annotation is not a proof (docs/0010 decision 6): it is
            // checked below, after the proof is in hand, and it never
            // appears on the left of an assignment to a type.
            for (const auto& param : fnDecl->params) {
                fn.params.push_back({param.name, il::Type::Dynamic});
            }
            fn.returnType = il::Type::Void;
            // The module function index: the position among the top-level
            // declarations, which is exactly how inference numbers them.
            const uint32_t moduleFnIndex = static_cast<uint32_t>(ilModule_.functions.size());
            if (!applyProvenSignature(*fnDecl, moduleFnIndex, fn)) return std::nullopt;
            // Now the hints, against the proof that just typed the signature.
            // Source order — parameters left to right, then the return —
            // and the enclosing loop is in source order too, so the warning
            // stream is deterministic.
            for (size_t i = 0; i < fnDecl->params.size(); ++i) {
                if (!checkAnnotation(fnDecl->params[i].typeAnnotation, fnDecl->span,
                                     fnDecl->params[i].name, provenParamType(moduleFnIndex, i))) {
                    return std::nullopt;
                }
            }
            // A return annotation is reported on the function's own name:
            // there is no other name for the position.
            if (!checkAnnotation(fnDecl->returnType, fnDecl->span, fnDecl->name,
                                 provenReturnType(moduleFnIndex))) {
                return std::nullopt;
            }
            // Every module function's return type is settled here, before
            // ANY body is lowered, because it is part of the calling
            // convention: `lowerCall` reads it off this entry, and for
            // mutual recursion it reads it while the callee's body is still
            // unlowered. Left to `lowerReturnStmt` to discover from the
            // first `return` it happens to reach, that read finds `Void` —
            // "returns nothing" — and the caller emits `ret` of a value
            // that does not exist, which the IL verifier rejects.
            //
            // Inference may already have pinned something better (an f64
            // return for a direct-callable function). What is left is a
            // function whose callers are unknown, and the sound convention
            // for those is the uniform dynamic one: `dynamic` if the body
            // can return a value at all, `void` if it demonstrably cannot.
            // That also retires first-return-wins, which was a live
            // miscompile of its own — `return 1; ... return "s"` unboxed a
            // string pointer as a double.
            if (fn.returnType == il::Type::Void && ast::returnsAValue(fnDecl->body)) {
                fn.returnType = il::Type::Dynamic;
            }
            fn.valueCount = static_cast<uint32_t>(fn.params.size());
            functionIndices_[fn.name] = moduleFnIndex;
            ilModule_.functions.push_back(std::move(fn));
        } else {
            topLevelStmts.push_back(stmtPtr.get());
        }
    }

    size_t fnIndex = 0;
    for (const auto& stmtPtr : astModule_.body) {
        if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmtPtr.get())) {
            // Lowered in place: a recursive call reads this very entry
            // for its arity and its still-being-inferred return type.
            // Safe only because Module::functions is reference-stable —
            // lowering this body can append nested closures to it.
            if (!lowerFunctionBody(*fnDecl, ilModule_.functions[fnIndex])) {
                return std::nullopt;
            }
            fnIndex++;
        }
    }

    if (!topLevelStmts.empty()) {
        il::Function mainFn;
        mainFn.name = "main";
        mainFn.returnType = il::Type::Void;
        mainFn.valueCount = 0;
        mainFn.blocks.push_back(il::Block{.id = 0});
        currentBlockIdx_ = 0;

        // The module top level is a function body like any other: its
        // variables can be captured by closures written at top level, so it
        // gets its environment record from the same rule (docs/0007).
        enterFunctionEnv(/*params=*/{}, topLevelStmts, mainFn);

        if (!lowerStmtList(topLevelStmts, mainFn)) {
            return std::nullopt;
        }
        auto& insts = mainFn.blocks[currentBlockIdx_].instructions;
        if (insts.empty() || !il::isTerminator(insts.back().op)) {
            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            retInst.type = il::Type::Void;
            emitInst(mainFn, retInst);
        }
        ilModule_.functions.push_back(std::move(mainFn));
    }

    if (diags_.hasErrors()) return std::nullopt;
    ilModule_.keyConstants.resize(keyConstants_.size());
    for (const auto& entry : keyConstants_) {
        ilModule_.keyConstants[entry.second] = entry.first;
    }
    // The site counter is now the size of a real allocation: the backend
    // emits exactly this many IC entries as a global array in the object
    // file (docs/0010 decision 7).
    ilModule_.icSiteCount = icSiteCounter_;
    return ilModule_;
}

// One function's environment record: the slots for every name the function
// declares that some nested function references, in a fixed source order
// (docs/0007 decisions 1 and 2). The module top level takes exactly the same
// rule with no parameters, which is why this is one function and not two —
// two copies of "captured names intersected with this scope's declarations,
// then the hoisted vars" is two copies that can drift, and a drift between
// them is a closure reading a slot the declaring scope never allocated.
//
// Must run after `currentEnvValue_` names the environment this one chains
// to: `emitEnvCreate` reads it as the parent link.
void Lowerer::enterFunctionEnv(const std::vector<ast::Param>& params,
                               const std::vector<const ast::Stmt*>& body, il::Function& ilFn) {
    // Which of this function's own variables must live in an environment
    // record. The environment STACK is not cleared with it: enclosing
    // scopes' environments are how this function's free variables resolve.
    capturedNames_ = ast::getCapturedNames(body);
    functionEnvBase_ = envScopes_.size();
    functionEnvScope_ = SIZE_MAX;

    std::vector<std::string> slots;
    auto addSlot = [&](const std::string& slotName) {
        if (!capturedNames_.contains(slotName)) return;
        if (std::find(slots.begin(), slots.end(), slotName) != slots.end()) return;
        slots.push_back(slotName);
    };
    // Parameters first, then the body's own let/const/function declarations,
    // then `var`s hoisted from anywhere below (they are function-scoped
    // wherever they are written, so they belong to this record and not to
    // the block that spells them).
    for (const auto& p : params) addSlot(p.name);
    for (const auto& declName : ast::getScopeDeclarations(body)) addSlot(declName);
    for (const auto& varName : ast::getHoistedVarDeclarations(body)) addSlot(varName);
    // The receiver, when an arrow in this body reads it (docs/0011 decision
    // 6). It is not a declaration, so nothing above finds it.
    addSlot("this");
    if (slots.empty()) return;

    EnvScopeInfo info;
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    functionEnvScope_ = envScopes_.size() - 1;
}

bool Lowerer::lowerFunctionBody(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body, il::Function& ilFn) {
    ilFn.blocks.push_back(il::Block{.id = 0});
    currentBlockIdx_ = 0;
    varBindings_.clear();
    activeVarMap_.clear();
    currentScopeDepth_ = 0;
    varDeclCounter_ = 0;
    loopStack_.clear();
    scopeHasEnv_.clear();

    // Synthetic parameters lead: [__env?][__this?] then source params.
    const uint32_t paramBase = static_cast<uint32_t>(ilFn.firstSourceParam());
    if (ilFn.needsEnv) currentEnvValue_ = 0;
    currentThisValue_ = ilFn.needsThis ? (ilFn.needsEnv ? 1u : 0u) : il::kNoValue;

    std::vector<const ast::Stmt*> stmts;
    stmts.reserve(body.size());
    for (const auto& s : body) stmts.push_back(s.get());
    enterFunctionEnv(params, stmts, ilFn);

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

    for (uint32_t i = 0; i < params.size(); ++i) {
        declareVariable(params[i].name, ilFn.params[i + paramBase].type, /*isConst=*/false,
                        /*isLet=*/false, /*isVar=*/false, /*isInitialized=*/true, i + paramBase,
                        Span{});
        // A captured parameter arrives in a register; copy it into its
        // environment slot so closures see the same binding.
        VarBinding& pb = varBindings_[activeVarMap_[params[i].name]];
        if (pb.inEnv) {
            emitEnvSet(envDepthOf(pb.envScopeIndex), pb.envSlot,
                       Value{i + paramBase, ilFn.params[i + paramBase].type}, ilFn);
        }
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

// The return annotation is deliberately not passed down: it is a hint, and
// the callers apply it — `lower()` and `lowerClosure` both check it against
// the proof BEFORE the body is lowered, because the IL return type is part
// of the calling convention (docs/0010 decision 6).
bool Lowerer::lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
    return lowerFunctionBody(fnDecl.params, fnDecl.body, ilFn);
}

std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags,
                                      const types::InferenceResult* inference) {
    Lowerer lowerer(astModule, diags, inference);
    return lowerer.lower();
}

}  // namespace bronze::lower
