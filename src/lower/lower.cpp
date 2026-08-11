#include "lower/lower.h"

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<il::Type> mapTypeAnnotation(const std::string& ann, Span span, DiagnosticSink& diags) {
    if (ann == "number" || ann == "f64") return il::Type::F64;
    if (ann == "bool" || ann == "boolean") return il::Type::Bool;
    if (ann == "void") return il::Type::Void;
    if (ann == "i32") return il::Type::I32;
    if (ann == "str") return il::Type::Str;
    if (ann == "dynamic" || ann == "any") return il::Type::Dynamic;
    if (ann.empty()) return il::Type::F64;
    diags.error(span, "unsupported type annotation: " + ann);
    return std::nullopt;
}

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
            for (const auto& param : fnDecl->params) {
                if (!param.typeAnnotation.empty()) {
                    auto pType = mapTypeAnnotation(param.typeAnnotation, fnDecl->span, diags_);
                    if (!pType) return std::nullopt;
                    fn.params.push_back({param.name, *pType});
                } else {
                    fn.params.push_back({param.name, il::Type::Dynamic});
                }
            }
            if (!fnDecl->returnType.empty()) {
                auto rType = mapTypeAnnotation(fnDecl->returnType, fnDecl->span, diags_);
                if (!rType) return std::nullopt;
                fn.returnType = *rType;
            } else {
                fn.returnType = il::Type::Void;
            }
            fn.valueCount = static_cast<uint32_t>(fn.params.size());
            functionIndices_[fn.name] = static_cast<uint32_t>(ilModule_.functions.size());
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
        // variables can be captured by closures written at top level.
        capturedNames_ = ast::getCapturedNames(topLevelStmts);
        functionEnvBase_ = envScopes_.size();
        functionEnvScope_ = SIZE_MAX;
        std::vector<std::string> mainEnvSlots;
        for (const auto& declName : ast::getScopeDeclarations(topLevelStmts)) {
            if (capturedNames_.contains(declName)) mainEnvSlots.push_back(declName);
        }
        for (const auto& varName : ast::getHoistedVarDeclarations(topLevelStmts)) {
            if (capturedNames_.contains(varName) &&
                std::find(mainEnvSlots.begin(), mainEnvSlots.end(), varName) ==
                    mainEnvSlots.end()) {
                mainEnvSlots.push_back(varName);
            }
        }
        if (!mainEnvSlots.empty()) {
            EnvScopeInfo info;
            for (uint32_t i = 0; i < mainEnvSlots.size(); ++i) info.slotOf[mainEnvSlots[i]] = i;
            info.envValue = emitEnvCreate(static_cast<uint32_t>(mainEnvSlots.size()), mainFn);
            envScopes_.push_back(std::move(info));
            savedEnvValues_.push_back(currentEnvValue_);
            currentEnvValue_ = envScopes_.back().envValue;
            functionEnvScope_ = envScopes_.size() - 1;
        }

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
    return ilModule_;
}

bool Lowerer::lowerFunctionBody(const std::string& name, const std::vector<ast::Param>& params,
                                const std::string& returnTypeAnnotation,
                                const std::vector<ast::StmtPtr>& body, il::Function& ilFn) {
    (void)name;
    (void)returnTypeAnnotation;
    ilFn.blocks.push_back(il::Block{.id = 0});
    currentBlockIdx_ = 0;
    varBindings_.clear();
    activeVarMap_.clear();
    currentScopeDepth_ = 0;
    varDeclCounter_ = 0;
    loopStack_.clear();
    scopeHasEnv_.clear();

    // Which of this function's own variables must live in an
    // environment record (docs/0007). The environment stack itself is
    // NOT cleared: enclosing scopes' environments are how this
    // function's free variables resolve.
    capturedNames_ = ast::getCapturedNames(body);
    functionEnvBase_ = envScopes_.size();
    functionEnvScope_ = SIZE_MAX;

    // Synthetic parameters lead: [__env?][__this?] then source params.
    const uint32_t paramBase = static_cast<uint32_t>(ilFn.firstSourceParam());
    if (ilFn.needsEnv) currentEnvValue_ = 0;
    currentThisValue_ = ilFn.needsThis ? (ilFn.needsEnv ? 1u : 0u) : il::kNoValue;

    std::vector<std::string> envSlots;
    for (const auto& p : params) {
        if (capturedNames_.contains(p.name)) envSlots.push_back(p.name);
    }
    for (const auto& declName : ast::getScopeDeclarations(body)) {
        if (capturedNames_.contains(declName)) envSlots.push_back(declName);
    }
    for (const auto& varName : ast::getHoistedVarDeclarations(body)) {
        if (capturedNames_.contains(varName) &&
            std::find(envSlots.begin(), envSlots.end(), varName) == envSlots.end()) {
            envSlots.push_back(varName);
        }
    }
    if (!envSlots.empty()) {
        EnvScopeInfo info;
        for (uint32_t i = 0; i < envSlots.size(); ++i) info.slotOf[envSlots[i]] = i;
        info.envValue = emitEnvCreate(static_cast<uint32_t>(envSlots.size()), ilFn);
        envScopes_.push_back(std::move(info));
        savedEnvValues_.push_back(currentEnvValue_);
        currentEnvValue_ = envScopes_.back().envValue;
        functionEnvScope_ = envScopes_.size() - 1;
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

    std::vector<const ast::Stmt*> stmts;
    for (const auto& s : body) stmts.push_back(s.get());
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

bool Lowerer::lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
    return lowerFunctionBody(fnDecl.name, fnDecl.params, fnDecl.returnType, fnDecl.body, ilFn);
}

std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags) {
    Lowerer lowerer(astModule, diags);
    return lowerer.lower();
}

}  // namespace bronze::lower
