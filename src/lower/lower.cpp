#include "lower/lower.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lower/assigned_set.h"
#include "ast/queries.h"

namespace bronze::lower {
namespace {

static std::optional<il::Type> mapTypeAnnotation(const std::string& ann, Span span, DiagnosticSink& diags) {
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

class Lowerer {
public:
    Lowerer(const ast::Module& astModule, DiagnosticSink& diags)
        : astModule_(astModule), diags_(diags) {}

    std::optional<il::Module> lower() {
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

private:
    const ast::Module& astModule_;
    DiagnosticSink& diags_;
    il::Module ilModule_;
    std::unordered_map<std::string, uint32_t> functionIndices_;
    std::unordered_map<std::string, uint32_t> keyConstants_;
    uint32_t icSiteCounter_ = 0;

    struct Value {
        il::ValueId id;
        il::Type type;
    };

    struct VarBinding {
        std::string name;
        il::Type type = il::Type::Dynamic;
        bool isConst = false;
        bool isLet = false;
        bool isVar = false;
        bool isInitialized = true;
        uint32_t declOrder = 0;
        size_t scopeDepth = 0;
        il::ValueId valueId = il::kNoValue;
        // Captured by some nested function, so it lives in an environment
        // record instead of in SSA (docs/0007). Reads become env.get and
        // writes env.set, and it takes no part in SSA joins.
        bool inEnv = false;
        size_t envScopeIndex = 0;
        uint32_t envSlot = 0;
    };

    struct LoopContext {
        il::BlockId headerBlock = il::kNoBlock;
        il::BlockId updateBlock = il::kNoBlock;
        il::BlockId exitBlock = il::kNoBlock;
        std::vector<std::string> loopVars;
    };

    std::vector<VarBinding> varBindings_;
    std::unordered_map<std::string, size_t> activeVarMap_;
    size_t currentScopeDepth_ = 0;
    uint32_t varDeclCounter_ = 0;
    std::vector<LoopContext> loopStack_;
    size_t currentBlockIdx_ = 0;

    // --- environments (docs/0007) ---------------------------------------
    // One entry per open scope that declares a captured variable, innermost
    // last. The stack spans function boundaries: that is exactly how a
    // nested function resolves a free variable to a (depth, index) pair
    // relative to the environment it is handed at entry.
    struct EnvScopeInfo {
        std::unordered_map<std::string, uint32_t> slotOf;
        il::ValueId envValue = il::kNoValue;  // meaningful only in the owning function
    };
    std::vector<EnvScopeInfo> envScopes_;
    std::vector<il::ValueId> savedEnvValues_;
    std::vector<bool> scopeHasEnv_;
    il::ValueId currentEnvValue_ = il::kNoValue;
    // The `__this` parameter of the function being lowered, or kNoValue
    // where there is no receiver to speak of (docs/0008 decision 3).
    il::ValueId currentThisValue_ = il::kNoValue;
    std::unordered_set<std::string> capturedNames_;
    size_t functionEnvBase_ = 0;   // envScopes_ size on entry to this function
    size_t functionEnvScope_ = SIZE_MAX;  // this function's own scope, if it has one

    uint32_t getKeyConstantIndex(const std::string& key) {
        auto it = keyConstants_.find(key);
        if (it != keyConstants_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(keyConstants_.size());
        keyConstants_[key] = idx;
        return idx;
    }

    il::BlockId createBlock(il::Function& ilFn) {
        il::BlockId id = static_cast<il::BlockId>(ilFn.blocks.size());
        ilFn.blocks.push_back(il::Block{.id = id});
        return id;
    }

    void setCurrentBlock(size_t blockIdx) {
        currentBlockIdx_ = blockIdx;
    }

    void emitInst(il::Function& ilFn, const il::Instruction& inst) {
        if (currentBlockIdx_ >= ilFn.blocks.size()) {
            ilFn.blocks.push_back(il::Block{.id = static_cast<il::BlockId>(currentBlockIdx_)});
        }
        ilFn.blocks[currentBlockIdx_].instructions.push_back(inst);
    }

    bool currentBlockIsTerminated(const il::Function& ilFn) const {
        if (currentBlockIdx_ >= ilFn.blocks.size()) return false;
        const auto& insts = ilFn.blocks[currentBlockIdx_].instructions;
        return !insts.empty() && il::isTerminator(insts.back().op);
    }

    Value boxValueIfNeeded(Value val, il::Function& ilFn) {
        if (val.type == il::Type::Dynamic) return val;
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Box;
        inst.type = il::Type::Dynamic;
        inst.boxType = val.type;
        inst.result = res;
        inst.operands = {val.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    Value unboxValueIfNeeded(Value val, il::Type targetType, il::Function& ilFn) {
        if (val.type == targetType) return val;
        if (val.type == il::Type::Bool && targetType == il::Type::F64) {
            // ToNumber(bool) — route through the boxed form.
            val = boxValueIfNeeded(val, ilFn);
        }
        if (val.type == il::Type::Dynamic) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Unbox;
            inst.type = targetType;
            inst.result = res;
            inst.operands = {val.id};
            emitInst(ilFn, inst);
            return Value{res, targetType};
        }
        diags_.error(Span{}, std::string("cannot convert ") + il::typeName(val.type) + " to " +
                                 il::typeName(targetType));
        return val;
    }

    // Combine the pre-read target value with the rhs of a compound
    // assignment. += routes through the dynamic add (JS + may
    // concatenate); the other operators are numeric.
    Value emitCompoundCombine(Value cur, Value rhs, ast::BinaryOp binOp, il::Function& ilFn) {
        if (binOp == ast::BinaryOp::PlusAssign) {
            Value l = boxValueIfNeeded(cur, ilFn);
            Value r = boxValueIfNeeded(rhs, ilFn);
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Add;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {l.id, r.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }
        il::Op op = il::Op::Sub;
        switch (binOp) {
            case ast::BinaryOp::MinusAssign: op = il::Op::Sub; break;
            case ast::BinaryOp::StarAssign: op = il::Op::Mul; break;
            case ast::BinaryOp::SlashAssign: op = il::Op::Div; break;
            case ast::BinaryOp::PercentAssign: op = il::Op::Mod; break;
            default:
                diags_.error(Span{}, "unsupported compound assignment operator");
                return cur;
        }
        Value l = unboxValueIfNeeded(cur, il::Type::F64, ilFn);
        Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = op;
        inst.type = il::Type::F64;
        inst.result = res;
        inst.operands = {l.id, r.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::F64};
    }

    // Conform a value flowing along a branch edge to the target block
    // parameter's type. Box into dynamic params; unbox out of dynamic
    // values (runtime-checked); anything else is a type conflict.
    Value coerceToType(Value val, il::Type target, il::Function& ilFn) {
        if (val.type == target) return val;
        if (target == il::Type::Dynamic) return boxValueIfNeeded(val, ilFn);
        return unboxValueIfNeeded(val, target, ilFn);
    }

    // Current values of the given variables, coerced (in the current block)
    // to the target block's parameter types, in parameter order.
    std::vector<il::ValueId> collectEdgeArgs(const std::vector<std::string>& vars,
                                             il::BlockId target, il::Function& ilFn) {
        std::vector<il::ValueId> args;
        for (size_t i = 0; i < vars.size(); ++i) {
            const auto& b = varBindings_[activeVarMap_[vars[i]]];
            args.push_back(coerceToType(Value{b.valueId, b.type},
                                        ilFn.blocks[target].params[i].type, ilFn).id);
        }
        return args;
    }

    // Env-backed variables are memory, not SSA, so they never become join
    // or loop-header parameters. This is the single funnel every join uses.
    std::vector<std::string> getActiveVarsInDeclOrder() const {
        std::vector<const VarBinding*> active;
        for (const auto& entry : activeVarMap_) {
            if (varBindings_[entry.second].inEnv) continue;
            active.push_back(&varBindings_[entry.second]);
        }
        std::sort(active.begin(), active.end(), [](const VarBinding* a, const VarBinding* b) {
            return a->declOrder < b->declOrder;
        });
        std::vector<std::string> names;
        for (const auto* b : active) {
            names.push_back(b->name);
        }
        return names;
    }

    // --- Conditional-expression joins -----------------------------------
    // &&, ||, ?? and ternary evaluate an operand on only some paths, so a
    // variable assigned inside such an operand needs a join parameter,
    // exactly like an if-statement arm. States are value snapshots because
    // assignments rebind varBindings_ entries in place.
    struct VarState {
        il::ValueId valueId;
        il::Type type;
    };
    using VarStateMap = std::unordered_map<std::string, VarState>;

    VarStateMap snapshotVarStates() const {
        VarStateMap snap;
        for (const auto& [name, idx] : activeVarMap_) {
            snap[name] = VarState{varBindings_[idx].valueId, varBindings_[idx].type};
        }
        return snap;
    }

    void restoreVarStates(const VarStateMap& snap) {
        for (const auto& [name, idx] : activeVarMap_) {
            varBindings_[idx].valueId = snap.at(name).valueId;
            varBindings_[idx].type = snap.at(name).type;
        }
    }

    struct ExprJoin {
        std::vector<std::string> vars;
        std::unordered_map<std::string, il::ValueId> paramId;
        std::unordered_map<std::string, il::Type> paramType;
    };

    // Join parameters for every variable whose value differs between the
    // two incoming states, appended after any params already on the join
    // block (the expression's result param comes first by convention).
    ExprJoin makeExprJoin(const VarStateMap& a, const VarStateMap& b,
                          il::BlockId joinBlock, il::Function& ilFn) {
        ExprJoin join;
        for (const auto& name : getActiveVarsInDeclOrder()) {
            if (a.at(name).valueId != b.at(name).valueId) join.vars.push_back(name);
        }
        for (const auto& name : join.vars) {
            il::ValueId pId = ilFn.valueCount++;
            il::Type tA = a.at(name).type;
            il::Type tB = b.at(name).type;
            il::Type pType = (tA == tB) ? tA : il::Type::Dynamic;
            ilFn.blocks[joinBlock].params.push_back({pId, pType});
            join.paramId[name] = pId;
            join.paramType[name] = pType;
        }
        return join;
    }

    // Coerce (in the current block) one incoming state's values to the join
    // param types and append them to that edge's argument list.
    void appendExprJoinArgs(std::vector<il::ValueId>& args, const ExprJoin& join,
                            const VarStateMap& state, il::Function& ilFn) {
        for (const auto& name : join.vars) {
            Value v{state.at(name).valueId, state.at(name).type};
            args.push_back(coerceToType(v, join.paramType.at(name), ilFn).id);
        }
    }

    void bindExprJoinParams(const ExprJoin& join) {
        for (const auto& name : join.vars) {
            auto& b = varBindings_[activeVarMap_[name]];
            b.valueId = join.paramId.at(name);
            b.type = join.paramType.at(name);
        }
    }

    bool declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet, bool isVar,
                         bool isInitialized, il::ValueId valId, Span span) {
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

    il::ValueId emitConstUndefined(il::Function& ilFn) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstUndefined;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return res;
    }

    il::ValueId emitEnvCreate(uint32_t slotCount, il::Function& ilFn) {
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

    Value emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn) {
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

    void emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn) {
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

    uint32_t envDepthOf(size_t scopeIndex) const {
        return static_cast<uint32_t>(envScopes_.size() - 1 - scopeIndex);
    }

    Value readBinding(const VarBinding& b, il::Function& ilFn) {
        if (!b.inEnv) return Value{b.valueId, b.type};
        return emitEnvGet(envDepthOf(b.envScopeIndex), b.envSlot, ilFn);
    }

    void writeBinding(VarBinding& b, Value val, il::Function& ilFn) {
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
    bool findEnclosingEnvVar(const std::string& name, uint32_t& depth, uint32_t& index) const {
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

    void enterScope() {
        currentScopeDepth_++;
        scopeHasEnv_.push_back(false);
    }

    // Scope entry that first gives the scope an environment record if any
    // of its own declarations are captured. For a loop body this runs once
    // per iteration at runtime, which is exactly the per-iteration binding
    // the language specifies.
    void enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn) {
        currentScopeDepth_++;
        std::vector<std::string> slots;
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

    void exitScope() {
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

    Value lowerCondition(const ast::Expr& expr, il::Function& ilFn) {
        auto valOpt = lowerExpr(expr, ilFn);
        if (!valOpt) return Value{il::kNoValue, il::Type::Bool};
        return lowerConditionFromVal(*valOpt, ilFn);
    }

    Value lowerConditionFromVal(Value val, il::Function& ilFn) {
        if (val.type == il::Type::Bool) return val;
        if (val.type == il::Type::F64) {
            il::ValueId zeroRes = ilFn.valueCount++;
            il::Instruction zeroInst;
            zeroInst.op = il::Op::ConstF64;
            zeroInst.type = il::Type::F64;
            zeroInst.result = zeroRes;
            zeroInst.immF64 = 0.0;
            emitInst(ilFn, zeroInst);

            il::ValueId cmpRes = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpNe;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = cmpRes;
            cmpInst.operands = {val.id, zeroRes};
            emitInst(ilFn, cmpInst);
            return Value{cmpRes, il::Type::Bool};
        }
        if (val.type == il::Type::I32) {
            il::ValueId zeroRes = ilFn.valueCount++;
            il::Instruction zeroInst;
            zeroInst.op = il::Op::ConstI32;
            zeroInst.type = il::Type::I32;
            zeroInst.result = zeroRes;
            zeroInst.immI32 = 0;
            emitInst(ilFn, zeroInst);

            il::ValueId cmpRes = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpNe;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = cmpRes;
            cmpInst.operands = {val.id, zeroRes};
            emitInst(ilFn, cmpInst);
            return Value{cmpRes, il::Type::Bool};
        }
        // Dynamic
        return unboxValueIfNeeded(val, il::Type::Bool, ilFn);
    }

    bool lowerFunctionBody(const std::string& name, const std::vector<ast::Param>& params,
                           const std::string& returnTypeAnnotation, const std::vector<ast::StmtPtr>& body,
                           il::Function& ilFn) {
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

    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
        return lowerFunctionBody(fnDecl.name, fnDecl.params, fnDecl.returnType, fnDecl.body, ilFn);
    }

    // Shared by function expressions and nested function declarations:
    // both produce a closure value over the environment that is innermost
    // at the creation site (docs/0007 decision 4).
    std::optional<Value> lowerClosure(const std::string& declaredName,
                                      const std::vector<ast::Param>& params,
                                      const std::string& returnTypeAnn,
                                      const std::vector<ast::StmtPtr>& body, Span span,
                                      il::Function& ilFn) {
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
            if (ast::usesThis(body)) {
                newFn.needsThis = true;
                newFn.params.push_back({"__this", il::Type::Dynamic});
            }
            for (const auto& param : params) {
                if (!param.typeAnnotation.empty()) {
                    auto pType = mapTypeAnnotation(param.typeAnnotation, span, diags_);
                    if (!pType) return std::nullopt;
                    newFn.params.push_back({param.name, *pType});
                } else {
                    newFn.params.push_back({param.name, il::Type::Dynamic});
                }
            }
            if (!returnTypeAnn.empty()) {
                auto rType = mapTypeAnnotation(returnTypeAnn, span, diags_);
                if (!rType) return std::nullopt;
                newFn.returnType = *rType;
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
            auto outerEnvBase = functionEnvBase_;
            auto outerEnvScope = functionEnvScope_;
            size_t outerEnvDepth = envScopes_.size();

            if (!lowerFunctionBody(fnName, params, returnTypeAnn, body, newFn)) {
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

    bool lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn) {
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

    bool lowerStmt(const ast::Stmt& stmt, il::Function& ilFn) {
        if (const auto* blockStmt = dynamic_cast<const ast::BlockStmt*>(&stmt)) {
            enterScope(blockStmt->stmts, ilFn);
            std::vector<const ast::Stmt*> stmts;
            for (const auto& s : blockStmt->stmts) stmts.push_back(s.get());
            if (!lowerStmtList(stmts, ilFn)) return false;
            exitScope();
            return true;
        }

        if (const auto* varDecl = dynamic_cast<const ast::VarDecl*>(&stmt)) {
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

        if (const auto* retStmt = dynamic_cast<const ast::ReturnStmt*>(&stmt)) {
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

        if (const auto* exprStmt = dynamic_cast<const ast::ExprStmt*>(&stmt)) {
            auto val = lowerExpr(*exprStmt->expr, ilFn);
            if (!val) return false;
            return true;
        }

        if (const auto* ifStmt = dynamic_cast<const ast::IfStmt*>(&stmt)) {
            Value condVal = lowerCondition(*ifStmt->condition, ilFn);
            if (condVal.id == il::kNoValue) return false;

            il::BlockId bThen = createBlock(ilFn);
            il::BlockId bElse = createBlock(ilFn);
            il::BlockId bJoin = createBlock(ilFn);

            auto envBefore = activeVarMap_;
            size_t entryBlockIdx = currentBlockIdx_;

            // Assignments rebind varBindings_ entries in place, so each arm
            // must start from a value snapshot of the pre-if state, and the
            // join must compare snapshots, not the (shared) live slots.
            // (Snapshots here range over envBefore, not the live map, because
            // arm bodies enter and exit scopes.)
            auto snapshot = [&]() {
                VarStateMap snap;
                for (const auto& [name, idx] : envBefore) {
                    snap[name] = VarState{varBindings_[idx].valueId, varBindings_[idx].type};
                }
                return snap;
            };
            auto restore = [&](const std::unordered_map<std::string, VarState>& snap) {
                for (const auto& [name, idx] : envBefore) {
                    varBindings_[idx].valueId = snap.at(name).valueId;
                    varBindings_[idx].type = snap.at(name).type;
                }
            };
            auto stateBefore = snapshot();

            // Then branch
            setCurrentBlock(bThen);
            enterScope(ifStmt->thenBody, ilFn);
            std::vector<const ast::Stmt*> thenStmts;
            for (const auto& s : ifStmt->thenBody) thenStmts.push_back(s.get());
            if (!lowerStmtList(thenStmts, ilFn)) return false;
            exitScope();
            auto stateThenEnd = snapshot();
            bool thenReaches = !currentBlockIsTerminated(ilFn);
            size_t thenEndBlockIdx = currentBlockIdx_;

            // Else branch, from the pre-if state
            activeVarMap_ = envBefore;
            restore(stateBefore);
            setCurrentBlock(bElse);
            enterScope(ifStmt->elseBody, ilFn);
            std::vector<const ast::Stmt*> elseStmts;
            for (const auto& s : ifStmt->elseBody) elseStmts.push_back(s.get());
            if (!lowerStmtList(elseStmts, ilFn)) return false;
            exitScope();
            auto stateElseEnd = snapshot();
            bool elseReaches = !currentBlockIsTerminated(ilFn);
            size_t elseEndBlockIdx = currentBlockIdx_;

            // Emit branch instruction in entry block
            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condVal.id};
            brInst.target = il::BlockTarget{.block = bThen, .args = {}};
            brInst.elseTarget = il::BlockTarget{.block = bElse, .args = {}};
            ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

            activeVarMap_ = envBefore;

            if (!thenReaches && !elseReaches) {
                // Both arms terminated; bJoin stays as the (unreachable)
                // continuation so later statements have a home.
                restore(stateBefore);
                setCurrentBlock(bJoin);
                return true;
            }

            if (thenReaches != elseReaches) {
                // Single live edge: no join parameters, the reachable arm's
                // values flow through directly.
                const auto& liveState = thenReaches ? stateThenEnd : stateElseEnd;
                setCurrentBlock(thenReaches ? thenEndBlockIdx : elseEndBlockIdx);
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = {}};
                emitInst(ilFn, jmpInst);
                restore(liveState);
                setCurrentBlock(bJoin);
                return true;
            }

            // Both arms reach: join parameters for every variable whose value
            // differs between the arms; param type unifies the arm types
            // (boxing to dynamic on disagreement).
            std::vector<std::string> activeNames = getActiveVarsInDeclOrder();
            std::vector<std::string> joinVars;
            for (const auto& name : activeNames) {
                if (stateThenEnd.at(name).valueId != stateElseEnd.at(name).valueId) {
                    joinVars.push_back(name);
                }
            }

            std::unordered_map<std::string, il::ValueId> joinParamMap;
            std::unordered_map<std::string, il::Type> joinParamType;
            for (const auto& name : joinVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type tThen = stateThenEnd.at(name).type;
                il::Type tElse = stateElseEnd.at(name).type;
                il::Type pType = (tThen == tElse) ? tThen : il::Type::Dynamic;
                ilFn.blocks[bJoin].params.push_back({pId, pType});
                joinParamMap[name] = pId;
                joinParamType[name] = pType;
            }

            auto emitJoinEdge = [&](size_t endBlockIdx,
                                    const std::unordered_map<std::string, VarState>& state) {
                setCurrentBlock(endBlockIdx);
                std::vector<il::ValueId> args;
                for (const auto& name : joinVars) {
                    Value v{state.at(name).valueId, state.at(name).type};
                    args.push_back(coerceToType(v, joinParamType[name], ilFn).id);
                }
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                emitInst(ilFn, jmpInst);
            };
            emitJoinEdge(thenEndBlockIdx, stateThenEnd);
            emitJoinEdge(elseEndBlockIdx, stateElseEnd);

            setCurrentBlock(bJoin);
            restore(stateThenEnd);
            for (const auto& name : joinVars) {
                varBindings_[activeVarMap_[name]].valueId = joinParamMap[name];
                varBindings_[activeVarMap_[name]].type = joinParamType[name];
            }
            return true;
        }

        if (const auto* whileStmt = dynamic_cast<const ast::WhileStmt*>(&stmt)) {
            auto assignedSet = getAssignedVariables(*whileStmt);
            std::vector<std::string> activeNames = getActiveVarsInDeclOrder();
            std::vector<std::string> loopVars;
            for (const auto& name : activeNames) {
                if (assignedSet.contains(name)) {
                    loopVars.push_back(name);
                }
            }

            il::BlockId bHeader = createBlock(ilFn);
            il::BlockId bBody = createBlock(ilFn);
            il::BlockId bExit = createBlock(ilFn);

            // Jump to header from current block
            il::Instruction jmpEntry;
            jmpEntry.op = il::Op::Jump;
            jmpEntry.type = il::Type::Void;
            jmpEntry.result = il::kNoValue;
            std::vector<il::ValueId> entryArgs;
            for (const auto& name : loopVars) {
                entryArgs.push_back(varBindings_[activeVarMap_[name]].valueId);
            }
            jmpEntry.target = il::BlockTarget{.block = bHeader, .args = std::move(entryArgs)};
            emitInst(ilFn, jmpEntry);

            // Header params
            il::Block& headerBlock = ilFn.blocks[bHeader];
            std::unordered_map<std::string, il::ValueId> headerParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                headerBlock.params.push_back({pId, vType});
                headerParamMap[name] = pId;
            }

            setCurrentBlock(bHeader);
            for (const auto& name : loopVars) {
                varBindings_[activeVarMap_[name]].valueId = headerParamMap[name];
            }

            Value condVal = lowerCondition(*whileStmt->condition, ilFn);

            // Exit params
            il::Block& exitBlock = ilFn.blocks[bExit];
            std::unordered_map<std::string, il::ValueId> exitParamMap;
            std::vector<il::ValueId> headerExitArgs;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                exitBlock.params.push_back({pId, vType});
                exitParamMap[name] = pId;
                headerExitArgs.push_back(varBindings_[activeVarMap_[name]].valueId);
            }

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condVal.id};
            brInst.target = il::BlockTarget{.block = bBody, .args = {}};
            brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
            emitInst(ilFn, brInst);

            // Body
            setCurrentBlock(bBody);
            loopStack_.push_back(LoopContext{bHeader, bHeader, bExit, loopVars});
            enterScope(whileStmt->body, ilFn);
            std::vector<const ast::Stmt*> bodyStmts;
            for (const auto& s : whileStmt->body) bodyStmts.push_back(s.get());
            if (!lowerStmtList(bodyStmts, ilFn)) return false;
            exitScope();
            loopStack_.pop_back();

            if (!currentBlockIsTerminated(ilFn)) {
                il::Instruction backJmp;
                backJmp.op = il::Op::Jump;
                backJmp.type = il::Type::Void;
                backJmp.result = il::kNoValue;
                backJmp.target = il::BlockTarget{
                    .block = bHeader,
                    .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
                emitInst(ilFn, backJmp);
            }

            setCurrentBlock(bExit);
            for (size_t i = 0; i < loopVars.size(); ++i) {
                auto& b = varBindings_[activeVarMap_[loopVars[i]]];
                b.valueId = exitParamMap[loopVars[i]];
                b.type = ilFn.blocks[bExit].params[i].type;
            }
            return true;
        }

        if (const auto* doWhileStmt = dynamic_cast<const ast::DoWhileStmt*>(&stmt)) {
            auto assignedSet = getAssignedVariables(*doWhileStmt);
            std::vector<std::string> activeNames = getActiveVarsInDeclOrder();
            std::vector<std::string> loopVars;
            for (const auto& name : activeNames) {
                if (assignedSet.contains(name)) {
                    loopVars.push_back(name);
                }
            }

            il::BlockId bHeader = createBlock(ilFn);
            il::BlockId bCond = createBlock(ilFn);
            il::BlockId bExit = createBlock(ilFn);

            // Entry jump
            il::Instruction jmpEntry;
            jmpEntry.op = il::Op::Jump;
            jmpEntry.type = il::Type::Void;
            jmpEntry.result = il::kNoValue;
            std::vector<il::ValueId> entryArgs;
            for (const auto& name : loopVars) {
                entryArgs.push_back(varBindings_[activeVarMap_[name]].valueId);
            }
            jmpEntry.target = il::BlockTarget{.block = bHeader, .args = std::move(entryArgs)};
            emitInst(ilFn, jmpEntry);

            // Header params
            il::Block& headerBlock = ilFn.blocks[bHeader];
            std::unordered_map<std::string, il::ValueId> headerParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                headerBlock.params.push_back({pId, vType});
                headerParamMap[name] = pId;
            }

            setCurrentBlock(bHeader);
            for (const auto& name : loopVars) {
                varBindings_[activeVarMap_[name]].valueId = headerParamMap[name];
            }

            // Exit params
            il::Block& exitBlock = ilFn.blocks[bExit];
            std::unordered_map<std::string, il::ValueId> exitParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                exitBlock.params.push_back({pId, vType});
                exitParamMap[name] = pId;
            }

            // The condition block joins the body fall-through and continue
            // edges, so it takes the loop variables as parameters.
            std::unordered_map<std::string, il::ValueId> condParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                ilFn.blocks[bCond].params.push_back({pId, vType});
                condParamMap[name] = pId;
            }

            loopStack_.push_back(LoopContext{bHeader, bCond, bExit, loopVars});
            enterScope(doWhileStmt->body, ilFn);
            std::vector<const ast::Stmt*> bodyStmts;
            for (const auto& s : doWhileStmt->body) bodyStmts.push_back(s.get());
            if (!lowerStmtList(bodyStmts, ilFn)) return false;
            exitScope();
            loopStack_.pop_back();

            if (!currentBlockIsTerminated(ilFn)) {
                il::Instruction toCond;
                toCond.op = il::Op::Jump;
                toCond.type = il::Type::Void;
                toCond.result = il::kNoValue;
                toCond.target = il::BlockTarget{
                    .block = bCond, .args = collectEdgeArgs(loopVars, bCond, ilFn)};
                emitInst(ilFn, toCond);
            }

            setCurrentBlock(bCond);
            for (size_t i = 0; i < loopVars.size(); ++i) {
                auto& b = varBindings_[activeVarMap_[loopVars[i]]];
                b.valueId = condParamMap[loopVars[i]];
                b.type = ilFn.blocks[bCond].params[i].type;
            }
            Value condVal = lowerCondition(*doWhileStmt->condition, ilFn);

            std::vector<il::ValueId> condBackArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
            std::vector<il::ValueId> condExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condVal.id};
            brInst.target = il::BlockTarget{.block = bHeader, .args = std::move(condBackArgs)};
            brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(condExitArgs)};
            emitInst(ilFn, brInst);

            setCurrentBlock(bExit);
            for (size_t i = 0; i < loopVars.size(); ++i) {
                auto& b = varBindings_[activeVarMap_[loopVars[i]]];
                b.valueId = exitParamMap[loopVars[i]];
                b.type = ilFn.blocks[bExit].params[i].type;
            }
            return true;
        }

        if (const auto* forStmt = dynamic_cast<const ast::ForStmt*>(&stmt)) {
            // A `for (let i = ...)` header binding is copied per iteration in
            // JS, so a closure over it must capture a fresh binding each time
            // round. That needs the environment threaded across the back
            // edge, which the block-scope rule does not give for free
            // (docs/0007 decision 2) — diagnose it rather than silently
            // sharing one binding.
            if (const auto* initDecl = dynamic_cast<const ast::VarDecl*>(forStmt->init.get())) {
                if (capturedNames_.contains(initDecl->name)) {
                    diags_.error(forStmt->span,
                                 "unsupported construct: closure capturing the for-loop binding '" +
                                     initDecl->name +
                                     "' (per-iteration binding semantics); use a `let` declared "
                                     "inside the loop body");
                    return false;
                }
            }
            enterScope();
            if (forStmt->init) {
                if (!lowerStmt(*forStmt->init, ilFn)) return false;
            }

            auto assignedSet = getAssignedVariables(*forStmt);
            std::vector<std::string> activeNames = getActiveVarsInDeclOrder();
            std::vector<std::string> loopVars;
            for (const auto& name : activeNames) {
                if (assignedSet.contains(name)) {
                    loopVars.push_back(name);
                }
            }

            il::BlockId bHeader = createBlock(ilFn);
            il::BlockId bBody = createBlock(ilFn);
            il::BlockId bUpdate = createBlock(ilFn);
            il::BlockId bExit = createBlock(ilFn);

            // Entry jump
            il::Instruction jmpEntry;
            jmpEntry.op = il::Op::Jump;
            jmpEntry.type = il::Type::Void;
            jmpEntry.result = il::kNoValue;
            std::vector<il::ValueId> entryArgs;
            for (const auto& name : loopVars) {
                entryArgs.push_back(varBindings_[activeVarMap_[name]].valueId);
            }
            jmpEntry.target = il::BlockTarget{.block = bHeader, .args = std::move(entryArgs)};
            emitInst(ilFn, jmpEntry);

            // Header params
            il::Block& headerBlock = ilFn.blocks[bHeader];
            std::unordered_map<std::string, il::ValueId> headerParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                headerBlock.params.push_back({pId, vType});
                headerParamMap[name] = pId;
            }

            setCurrentBlock(bHeader);
            for (const auto& name : loopVars) {
                varBindings_[activeVarMap_[name]].valueId = headerParamMap[name];
            }

            // Exit params
            il::Block& exitBlock = ilFn.blocks[bExit];
            std::unordered_map<std::string, il::ValueId> exitParamMap;
            std::vector<il::ValueId> headerExitArgs;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                exitBlock.params.push_back({pId, vType});
                exitParamMap[name] = pId;
                headerExitArgs.push_back(varBindings_[activeVarMap_[name]].valueId);
            }

            if (forStmt->condition) {
                Value condVal = lowerCondition(*forStmt->condition, ilFn);
                il::Instruction brInst;
                brInst.op = il::Op::Branch;
                brInst.type = il::Type::Void;
                brInst.result = il::kNoValue;
                brInst.operands = {condVal.id};
                brInst.target = il::BlockTarget{.block = bBody, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
                emitInst(ilFn, brInst);
            } else {
                il::Instruction jmpBody;
                jmpBody.op = il::Op::Jump;
                jmpBody.type = il::Type::Void;
                jmpBody.result = il::kNoValue;
                jmpBody.target = il::BlockTarget{.block = bBody, .args = {}};
                emitInst(ilFn, jmpBody);
            }

            // The update block joins the body fall-through and continue
            // edges, so it takes the loop variables as parameters.
            std::unordered_map<std::string, il::ValueId> updateParamMap;
            for (const auto& name : loopVars) {
                il::ValueId pId = ilFn.valueCount++;
                il::Type vType = varBindings_[activeVarMap_[name]].type;
                ilFn.blocks[bUpdate].params.push_back({pId, vType});
                updateParamMap[name] = pId;
            }

            // Body
            setCurrentBlock(bBody);
            loopStack_.push_back(LoopContext{bHeader, bUpdate, bExit, loopVars});
            enterScope(forStmt->body, ilFn);
            std::vector<const ast::Stmt*> bodyStmts;
            for (const auto& s : forStmt->body) bodyStmts.push_back(s.get());
            if (!lowerStmtList(bodyStmts, ilFn)) return false;
            exitScope();
            loopStack_.pop_back();

            if (!currentBlockIsTerminated(ilFn)) {
                il::Instruction toUpdate;
                toUpdate.op = il::Op::Jump;
                toUpdate.type = il::Type::Void;
                toUpdate.result = il::kNoValue;
                toUpdate.target = il::BlockTarget{
                    .block = bUpdate, .args = collectEdgeArgs(loopVars, bUpdate, ilFn)};
                emitInst(ilFn, toUpdate);
            }

            // Update
            setCurrentBlock(bUpdate);
            for (size_t i = 0; i < loopVars.size(); ++i) {
                auto& b = varBindings_[activeVarMap_[loopVars[i]]];
                b.valueId = updateParamMap[loopVars[i]];
                b.type = ilFn.blocks[bUpdate].params[i].type;
            }
            if (forStmt->update) {
                if (!lowerExpr(*forStmt->update, ilFn)) return false;
            }
            il::Instruction backJmp;
            backJmp.op = il::Op::Jump;
            backJmp.type = il::Type::Void;
            backJmp.result = il::kNoValue;
            backJmp.target = il::BlockTarget{
                .block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
            emitInst(ilFn, backJmp);

            setCurrentBlock(bExit);
            for (size_t i = 0; i < loopVars.size(); ++i) {
                auto& b = varBindings_[activeVarMap_[loopVars[i]]];
                b.valueId = exitParamMap[loopVars[i]];
                b.type = ilFn.blocks[bExit].params[i].type;
            }
            exitScope();
            return true;
        }

        if (const auto* breakStmt = dynamic_cast<const ast::BreakStmt*>(&stmt)) {
            if (!breakStmt->label.empty()) {
                diags_.error(breakStmt->span, "unsupported construct: labeled break/continue");
                return false;
            }
            if (loopStack_.empty()) {
                diags_.error(breakStmt->span, "break statement outside of loop");
                return false;
            }
            const auto& loopCtx = loopStack_.back();
            il::Instruction jmpInst;
            jmpInst.op = il::Op::Jump;
            jmpInst.type = il::Type::Void;
            jmpInst.result = il::kNoValue;
            jmpInst.target = il::BlockTarget{
                .block = loopCtx.exitBlock,
                .args = collectEdgeArgs(loopCtx.loopVars, loopCtx.exitBlock, ilFn)};
            emitInst(ilFn, jmpInst);

            // Create dead block for any unreachable trailing instructions in the same scope
            il::BlockId deadBlock = createBlock(ilFn);
            setCurrentBlock(deadBlock);
            return true;
        }

        if (const auto* continueStmt = dynamic_cast<const ast::ContinueStmt*>(&stmt)) {
            if (!continueStmt->label.empty()) {
                diags_.error(continueStmt->span, "unsupported construct: labeled break/continue");
                return false;
            }
            if (loopStack_.empty()) {
                diags_.error(continueStmt->span, "continue statement outside of loop");
                return false;
            }
            const auto& loopCtx = loopStack_.back();
            il::Instruction jmpInst;
            jmpInst.op = il::Op::Jump;
            jmpInst.type = il::Type::Void;
            jmpInst.result = il::kNoValue;
            jmpInst.target = il::BlockTarget{
                .block = loopCtx.updateBlock,
                .args = collectEdgeArgs(loopCtx.loopVars, loopCtx.updateBlock, ilFn)};
            emitInst(ilFn, jmpInst);

            il::BlockId deadBlock = createBlock(ilFn);
            setCurrentBlock(deadBlock);
            return true;
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

    std::optional<Value> lowerExpr(const ast::Expr& expr, il::Function& ilFn) {
        if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstF64;
            inst.type = il::Type::F64;
            inst.result = res;
            inst.immF64 = numLit->value;
            emitInst(ilFn, inst);
            return Value{res, il::Type::F64};
        }

        if (const auto* boolLit = dynamic_cast<const ast::BoolLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstBool;
            inst.type = il::Type::Bool;
            inst.result = res;
            inst.immI32 = boolLit->value ? 1 : 0;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Bool};
        }

        if (dynamic_cast<const ast::NullLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstNull;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (dynamic_cast<const ast::UndefinedLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstUndefined;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (dynamic_cast<const ast::ThisExpr*>(&expr)) {
            if (currentThisValue_ == il::kNoValue) {
                // usesThis() decided the parameter, so reaching here means
                // `this` at module top level, where its value is a module
                // system question bronze has not answered yet (docs/0008).
                diags_.error(expr.span, "`this` outside a function is unsupported");
                return std::nullopt;
            }
            return Value{currentThisValue_, il::Type::Dynamic};
        }

        if (const auto* strLit = dynamic_cast<const ast::StringLit*>(&expr)) {
            uint32_t keyIdx = getKeyConstantIndex(strLit->value);
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Box;
            inst.type = il::Type::Dynamic;
            inst.boxType = il::Type::Str;
            inst.result = res;
            inst.keyIndex = keyIdx;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* objLit = dynamic_cast<const ast::ObjectLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::CreateObject;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            emitInst(ilFn, inst);

            for (const auto& prop : objLit->props) {
                auto valOpt = lowerExpr(*prop.value, ilFn);
                if (!valOpt) return std::nullopt;
                auto valBoxed = boxValueIfNeeded(*valOpt, ilFn);

                uint32_t keyIdx = getKeyConstantIndex(prop.key);
                uint32_t icIdx = icSiteCounter_++;

                il::Instruction setInst;
                setInst.op = il::Op::PropSet;
                setInst.type = il::Type::Void;
                setInst.result = il::kNoValue;
                setInst.operands = {res, valBoxed.id};
                setInst.keyIndex = keyIdx;
                setInst.icIndex = icIdx;
                emitInst(ilFn, setInst);
            }
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* arrLit = dynamic_cast<const ast::ArrayLit*>(&expr)) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::CreateArray;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.immI32 = static_cast<int32_t>(arrLit->elements.size());
            emitInst(ilFn, inst);

            for (size_t i = 0; i < arrLit->elements.size(); ++i) {
                auto elemOpt = lowerExpr(*arrLit->elements[i], ilFn);
                if (!elemOpt) return std::nullopt;
                auto elemBoxed = boxValueIfNeeded(*elemOpt, ilFn);

                uint32_t keyIdx = getKeyConstantIndex(std::to_string(i));
                uint32_t icIdx = icSiteCounter_++;

                il::Instruction setInst;
                setInst.op = il::Op::PropSet;
                setInst.type = il::Type::Void;
                setInst.result = il::kNoValue;
                setInst.operands = {res, elemBoxed.id};
                setInst.keyIndex = keyIdx;
                setInst.icIndex = icIdx;
                emitInst(ilFn, setInst);
            }
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* fnExpr = dynamic_cast<const ast::FunctionExpr*>(&expr)) {
            return lowerClosure(fnExpr->name, fnExpr->params, fnExpr->returnType, fnExpr->body,
                                fnExpr->span, ilFn);
        }

        if (const auto* ident = dynamic_cast<const ast::Ident*>(&expr)) {
            auto it = activeVarMap_.find(ident->name);
            if (it == activeVarMap_.end()) {
                // Global value properties (shadowable by local declarations,
                // hence checked only after the variable lookup misses).
                if (ident->name == "NaN" || ident->name == "Infinity") {
                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction inst;
                    inst.op = il::Op::ConstF64;
                    inst.type = il::Type::F64;
                    inst.result = res;
                    inst.immF64 = ident->name == "NaN"
                                      ? std::numeric_limits<double>::quiet_NaN()
                                      : std::numeric_limits<double>::infinity();
                    emitInst(ilFn, inst);
                    return Value{res, il::Type::F64};
                }
                if (ident->name == "undefined") {
                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction inst;
                    inst.op = il::Op::ConstUndefined;
                    inst.type = il::Type::Dynamic;
                    inst.result = res;
                    emitInst(ilFn, inst);
                    return Value{res, il::Type::Dynamic};
                }
                // A free variable of a nested function: resolved against
                // the environments of the enclosing scopes (docs/0007).
                uint32_t depth = 0;
                uint32_t index = 0;
                if (currentEnvValue_ != il::kNoValue &&
                    findEnclosingEnvVar(ident->name, depth, index)) {
                    return emitEnvGet(depth, index, ilFn);
                }
                // A top-level function declaration used as a value rather
                // than called: `new Point(...)`, `Point.prototype`, passing
                // it around. One object per declaration, so decorating it
                // in one place is visible in every other (docs/0008).
                auto fnIt = functionIndices_.find(ident->name);
                if (fnIt != functionIndices_.end()) {
                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction inst;
                    inst.op = il::Op::FunctionRef;
                    inst.type = il::Type::Dynamic;
                    inst.result = res;
                    inst.calleeIndex = fnIt->second;
                    emitInst(ilFn, inst);
                    return Value{res, il::Type::Dynamic};
                }
                diags_.error(ident->span, "undefined variable: " + ident->name);
                return std::nullopt;
            }
            const auto& b = varBindings_[it->second];
            if (!b.isInitialized) {
                if (b.isConst) {
                    diags_.error(ident->span, "use of 'const' binding before initialization");
                } else {
                    diags_.error(ident->span, "use of 'let' binding before initialization");
                }
                return std::nullopt;
            }
            return readBinding(b, ilFn);
        }

        if (const auto* un = dynamic_cast<const ast::Unary*>(&expr)) {
            if (un->op == ast::UnaryOp::Not) {
                Value condVal = lowerCondition(*un->operand, ilFn);
                il::ValueId falseVal = ilFn.valueCount++;
                il::Instruction falseInst;
                falseInst.op = il::Op::ConstBool;
                falseInst.type = il::Type::Bool;
                falseInst.result = falseVal;
                falseInst.immI32 = 0;
                emitInst(ilFn, falseInst);

                il::ValueId res = ilFn.valueCount++;
                il::Instruction cmpInst;
                cmpInst.op = il::Op::CmpEq;
                cmpInst.type = il::Type::Bool;
                cmpInst.result = res;
                cmpInst.operands = {condVal.id, falseVal};
                emitInst(ilFn, cmpInst);
                return Value{res, il::Type::Bool};
            }
            if (un->op == ast::UnaryOp::Negate) {
                auto valOpt = lowerExpr(*un->operand, ilFn);
                if (!valOpt) return std::nullopt;
                Value val = unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
                il::ValueId zeroRes = ilFn.valueCount++;
                il::Instruction zeroInst;
                zeroInst.op = il::Op::ConstF64;
                zeroInst.type = il::Type::F64;
                zeroInst.result = zeroRes;
                zeroInst.immF64 = 0.0;
                emitInst(ilFn, zeroInst);

                il::ValueId res = ilFn.valueCount++;
                il::Instruction subInst;
                subInst.op = il::Op::Sub;
                subInst.type = il::Type::F64;
                subInst.result = res;
                subInst.operands = {zeroRes, val.id};
                emitInst(ilFn, subInst);
                return Value{res, il::Type::F64};
            }
            if (un->op == ast::UnaryOp::Posate) {
                auto valOpt = lowerExpr(*un->operand, ilFn);
                if (!valOpt) return std::nullopt;
                return unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
            }
            if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
                un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
                const auto* ident = dynamic_cast<const ast::Ident*>(un->operand.get());
                if (!ident) {
                    diags_.error(un->span, "invalid update operand");
                    return std::nullopt;
                }
                auto it = activeVarMap_.find(ident->name);
                if (it == activeVarMap_.end()) {
                    diags_.error(ident->span, "undefined variable: " + ident->name);
                    return std::nullopt;
                }
                VarBinding& b = varBindings_[it->second];
                Value oldVal = readBinding(b, ilFn);
                Value numOld = unboxValueIfNeeded(oldVal, il::Type::F64, ilFn);

                il::ValueId oneRes = ilFn.valueCount++;
                il::Instruction oneInst;
                oneInst.op = il::Op::ConstF64;
                oneInst.type = il::Type::F64;
                oneInst.result = oneRes;
                oneInst.immF64 = 1.0;
                emitInst(ilFn, oneInst);

                il::ValueId newValId = ilFn.valueCount++;
                il::Instruction calcInst;
                calcInst.op = (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PostInc) ? il::Op::Add : il::Op::Sub;
                calcInst.type = il::Type::F64;
                calcInst.result = newValId;
                calcInst.operands = {numOld.id, oneRes};
                emitInst(ilFn, calcInst);

                writeBinding(b, Value{newValId, il::Type::F64}, ilFn);

                if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec) {
                    return Value{newValId, il::Type::F64};
                } else {
                    return numOld;
                }
            }
        }

        if (const auto* tern = dynamic_cast<const ast::Ternary*>(&expr)) {
            Value condVal = lowerCondition(*tern->condition, ilFn);
            if (condVal.id == il::kNoValue) return std::nullopt;

            il::BlockId bThen = createBlock(ilFn);
            il::BlockId bElse = createBlock(ilFn);
            il::BlockId bJoin = createBlock(ilFn);

            size_t entryBlockIdx = currentBlockIdx_;
            auto statePre = snapshotVarStates();

            setCurrentBlock(bThen);
            auto thenValOpt = lowerExpr(*tern->thenExpr, ilFn);
            if (!thenValOpt) return std::nullopt;
            auto stateThen = snapshotVarStates();
            bool thenReaches = !currentBlockIsTerminated(ilFn);
            size_t thenEndBlockIdx = currentBlockIdx_;

            restoreVarStates(statePre);
            setCurrentBlock(bElse);
            auto elseValOpt = lowerExpr(*tern->elseExpr, ilFn);
            if (!elseValOpt) return std::nullopt;
            auto stateElse = snapshotVarStates();
            bool elseReaches = !currentBlockIsTerminated(ilFn);
            size_t elseEndBlockIdx = currentBlockIdx_;

            il::Type joinType = il::Type::Dynamic;
            if (thenValOpt->type == elseValOpt->type) {
                joinType = thenValOpt->type;
            }

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condVal.id};
            brInst.target = il::BlockTarget{.block = bThen, .args = {}};
            brInst.elseTarget = il::BlockTarget{.block = bElse, .args = {}};
            ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

            il::ValueId resParamId = ilFn.valueCount++;
            ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
            ExprJoin join = makeExprJoin(stateThen, stateElse, bJoin, ilFn);

            auto emitArmEdge = [&](size_t endBlockIdx, Value v, const VarStateMap& state) {
                setCurrentBlock(endBlockIdx);
                if (joinType == il::Type::Dynamic) v = boxValueIfNeeded(v, ilFn);
                else if (v.type != joinType) v = unboxValueIfNeeded(v, joinType, ilFn);
                std::vector<il::ValueId> args{v.id};
                appendExprJoinArgs(args, join, state, ilFn);
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                emitInst(ilFn, jmpInst);
            };
            if (thenReaches) emitArmEdge(thenEndBlockIdx, *thenValOpt, stateThen);
            if (elseReaches) emitArmEdge(elseEndBlockIdx, *elseValOpt, stateElse);

            setCurrentBlock(bJoin);
            restoreVarStates(thenReaches || !elseReaches ? stateThen : stateElse);
            bindExprJoinParams(join);
            return Value{resParamId, joinType};
        }

        if (const auto* newExpr = dynamic_cast<const ast::NewExpr*>(&expr)) {
            if (newExpr->callee == "Float32Array" || newExpr->callee == "ArrayBuffer") {
                if (newExpr->args.size() != 1) {
                    diags_.error(newExpr->span, "unsupported construct: new " + newExpr->callee +
                                                    " expects exactly one argument");
                    return std::nullopt;
                }
                auto argVal = lowerExpr(*newExpr->args[0], ilFn);
                if (!argVal) return std::nullopt;
                auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = (newExpr->callee == "Float32Array") ? il::Op::CreateFloat32Array
                                                              : il::Op::CreateArrayBuffer;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.operands = {argBoxed.id};
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // Any other callee is an ordinary value: the whole ceremony
            // (prototype, instance shape, receiver, result rule) lives in
            // one runtime helper rather than in codegen — docs/0008
            // decision 4.
            ast::Ident calleeIdent;
            calleeIdent.name = newExpr->callee;
            calleeIdent.span = newExpr->span;
            auto calleeVal = lowerExpr(calleeIdent, ilFn);
            if (!calleeVal) return std::nullopt;

            std::vector<il::ValueId> operands;
            operands.push_back(boxValueIfNeeded(*calleeVal, ilFn).id);
            for (const auto& argPtr : newExpr->args) {
                auto argVal = lowerExpr(*argPtr, ilFn);
                if (!argVal) return std::nullopt;
                operands.push_back(boxValueIfNeeded(*argVal, ilFn).id);
            }

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Construct;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = std::move(operands);
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&expr)) {
            auto objVal = lowerExpr(*mem->object, ilFn);
            if (!objVal) return std::nullopt;
            auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

            uint32_t keyIdx = getKeyConstantIndex(mem->property);
            uint32_t icIdx = icSiteCounter_++;

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::PropGet;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {objBoxed.id};
            inst.keyIndex = keyIdx;
            inst.icIndex = icIdx;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(&expr)) {
            auto objVal = lowerExpr(*idxAccess->object, ilFn);
            if (!objVal) return std::nullopt;
            auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

            uint32_t keyIdx = 0;
            bool literalKey = true;
            if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(idxAccess->index.get())) {
                keyIdx = getKeyConstantIndex(std::to_string(static_cast<int64_t>(numLit->value)));
            } else if (const auto* strLit = dynamic_cast<const ast::StringLit*>(idxAccess->index.get())) {
                keyIdx = getKeyConstantIndex(strLit->value);
            } else {
                literalKey = false;
            }

            if (!literalKey) {
                // Computed index: a real elem.get on the index value.
                auto indexVal = lowerExpr(*idxAccess->index, ilFn);
                if (!indexVal) return std::nullopt;
                auto idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ElemGet;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.operands = {objBoxed.id, idxBoxed.id};
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }

            uint32_t icIdx = icSiteCounter_++;
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::PropGet;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {objBoxed.id};
            inst.keyIndex = keyIdx;
            inst.icIndex = icIdx;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* bin = dynamic_cast<const ast::Binary*>(&expr)) {
            if (bin->op == ast::BinaryOp::Assign || bin->op == ast::BinaryOp::PlusAssign ||
                bin->op == ast::BinaryOp::MinusAssign || bin->op == ast::BinaryOp::StarAssign ||
                bin->op == ast::BinaryOp::SlashAssign || bin->op == ast::BinaryOp::PercentAssign) {
                if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
                    auto objVal = lowerExpr(*mem->object, ilFn);
                    if (!objVal) return std::nullopt;
                    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
                    uint32_t keyIdx = getKeyConstantIndex(mem->property);

                    // Compound assignment reads the current value before the
                    // rhs is evaluated (JS evaluation order).
                    std::optional<Value> curVal;
                    if (bin->op != ast::BinaryOp::Assign) {
                        il::ValueId cur = ilFn.valueCount++;
                        il::Instruction getInst;
                        getInst.op = il::Op::PropGet;
                        getInst.type = il::Type::Dynamic;
                        getInst.result = cur;
                        getInst.operands = {objBoxed.id};
                        getInst.keyIndex = keyIdx;
                        getInst.icIndex = icSiteCounter_++;
                        emitInst(ilFn, getInst);
                        curVal = Value{cur, il::Type::Dynamic};
                    }

                    auto rhsVal = lowerExpr(*bin->rhs, ilFn);
                    if (!rhsVal) return std::nullopt;
                    Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op, ilFn)
                                          : *rhsVal;
                    Value storedBoxed = boxValueIfNeeded(stored, ilFn);

                    il::Instruction inst;
                    inst.op = il::Op::PropSet;
                    inst.type = il::Type::Void;
                    inst.result = il::kNoValue;
                    inst.operands = {objBoxed.id, storedBoxed.id};
                    inst.keyIndex = keyIdx;
                    inst.icIndex = icSiteCounter_++;
                    emitInst(ilFn, inst);
                    return storedBoxed;
                }
                if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(bin->lhs.get())) {
                    auto objVal = lowerExpr(*idxAccess->object, ilFn);
                    if (!objVal) return std::nullopt;
                    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

                    uint32_t keyIdx = 0;
                    bool literalKey = true;
                    if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(idxAccess->index.get())) {
                        keyIdx = getKeyConstantIndex(std::to_string(static_cast<int64_t>(numLit->value)));
                    } else if (const auto* strLit = dynamic_cast<const ast::StringLit*>(idxAccess->index.get())) {
                        keyIdx = getKeyConstantIndex(strLit->value);
                    } else {
                        literalKey = false;
                    }

                    std::optional<Value> idxBoxed;
                    if (!literalKey) {
                        auto indexVal = lowerExpr(*idxAccess->index, ilFn);
                        if (!indexVal) return std::nullopt;
                        idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
                    }

                    // Compound assignment reads the current element before
                    // the rhs is evaluated (JS evaluation order).
                    std::optional<Value> curVal;
                    if (bin->op != ast::BinaryOp::Assign) {
                        il::ValueId cur = ilFn.valueCount++;
                        il::Instruction getInst;
                        if (literalKey) {
                            getInst.op = il::Op::PropGet;
                            getInst.operands = {objBoxed.id};
                            getInst.keyIndex = keyIdx;
                            getInst.icIndex = icSiteCounter_++;
                        } else {
                            getInst.op = il::Op::ElemGet;
                            getInst.operands = {objBoxed.id, idxBoxed->id};
                        }
                        getInst.type = il::Type::Dynamic;
                        getInst.result = cur;
                        emitInst(ilFn, getInst);
                        curVal = Value{cur, il::Type::Dynamic};
                    }

                    auto rhsVal = lowerExpr(*bin->rhs, ilFn);
                    if (!rhsVal) return std::nullopt;
                    Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op, ilFn)
                                          : *rhsVal;
                    Value storedBoxed = boxValueIfNeeded(stored, ilFn);

                    il::Instruction setInst;
                    if (literalKey) {
                        setInst.op = il::Op::PropSet;
                        setInst.operands = {objBoxed.id, storedBoxed.id};
                        setInst.keyIndex = keyIdx;
                        setInst.icIndex = icSiteCounter_++;
                    } else {
                        setInst.op = il::Op::ElemSet;
                        setInst.operands = {objBoxed.id, idxBoxed->id, storedBoxed.id};
                    }
                    setInst.type = il::Type::Void;
                    setInst.result = il::kNoValue;
                    emitInst(ilFn, setInst);
                    return storedBoxed;
                }
                if (const auto* ident = dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
                    auto rhsVal = lowerExpr(*bin->rhs, ilFn);
                    if (!rhsVal) return std::nullopt;

                    auto it = activeVarMap_.find(ident->name);
                    if (it == activeVarMap_.end()) {
                        // Assignment to a variable captured from an
                        // enclosing scope: the closure writes through the
                        // environment, so the declaring scope sees it.
                        uint32_t depth = 0;
                        uint32_t index = 0;
                        if (currentEnvValue_ != il::kNoValue &&
                            findEnclosingEnvVar(ident->name, depth, index)) {
                            if (bin->op == ast::BinaryOp::Assign) {
                                emitEnvSet(depth, index, *rhsVal, ilFn);
                                return rhsVal;
                            }
                            Value lhsOuter = emitEnvGet(depth, index, ilFn);
                            il::Op outerOp;
                            switch (bin->op) {
                                case ast::BinaryOp::PlusAssign: outerOp = il::Op::Add; break;
                                case ast::BinaryOp::MinusAssign: outerOp = il::Op::Sub; break;
                                case ast::BinaryOp::StarAssign: outerOp = il::Op::Mul; break;
                                case ast::BinaryOp::SlashAssign: outerOp = il::Op::Div; break;
                                case ast::BinaryOp::PercentAssign: outerOp = il::Op::Mod; break;
                                default: outerOp = il::Op::Add; break;
                            }
                            Value lhsNum = unboxValueIfNeeded(lhsOuter, il::Type::F64, ilFn);
                            Value rhsNum = unboxValueIfNeeded(*rhsVal, il::Type::F64, ilFn);
                            il::ValueId res = ilFn.valueCount++;
                            il::Instruction inst;
                            inst.op = outerOp;
                            inst.type = il::Type::F64;
                            inst.result = res;
                            inst.operands = {lhsNum.id, rhsNum.id};
                            emitInst(ilFn, inst);
                            Value result{res, il::Type::F64};
                            emitEnvSet(depth, index, result, ilFn);
                            return result;
                        }
                        diags_.error(ident->span, "undefined variable: " + ident->name);
                        return std::nullopt;
                    }
                    VarBinding& b = varBindings_[it->second];

                    if (bin->op == ast::BinaryOp::Assign) {
                        writeBinding(b, *rhsVal, ilFn);
                        return rhsVal;
                    } else {
                        Value lhsVal = readBinding(b, ilFn);
                        il::Op op;
                        switch (bin->op) {
                            case ast::BinaryOp::PlusAssign: op = il::Op::Add; break;
                            case ast::BinaryOp::MinusAssign: op = il::Op::Sub; break;
                            case ast::BinaryOp::StarAssign: op = il::Op::Mul; break;
                            case ast::BinaryOp::SlashAssign: op = il::Op::Div; break;
                            case ast::BinaryOp::PercentAssign: op = il::Op::Mod; break;
                            default: op = il::Op::Add; break;
                        }
                        Value lhsNum = unboxValueIfNeeded(lhsVal, il::Type::F64, ilFn);
                        Value rhsNum = unboxValueIfNeeded(*rhsVal, il::Type::F64, ilFn);

                        il::ValueId res = ilFn.valueCount++;
                        il::Instruction inst;
                        inst.op = op;
                        inst.type = il::Type::F64;
                        inst.result = res;
                        inst.operands = {lhsNum.id, rhsNum.id};
                        emitInst(ilFn, inst);

                        writeBinding(b, Value{res, il::Type::F64}, ilFn);
                        return Value{res, il::Type::F64};
                    }
                }
                diags_.error(bin->span, "invalid assignment target");
                return std::nullopt;
            }

            if (bin->op == ast::BinaryOp::LogicalAnd || bin->op == ast::BinaryOp::LogicalOr) {
                auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
                if (!lhsOpt) return std::nullopt;
                Value lhsVal = *lhsOpt;

                Value lhsBool = lowerConditionFromVal(lhsVal, ilFn);

                il::BlockId bRhs = createBlock(ilFn);
                il::BlockId bJoin = createBlock(ilFn);
                size_t entryBlockIdx = currentBlockIdx_;
                auto stateLhs = snapshotVarStates();

                setCurrentBlock(bRhs);
                auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
                if (!rhsOpt) return std::nullopt;
                Value rhsVal = *rhsOpt;
                auto stateRhs = snapshotVarStates();
                bool rhsReaches = !currentBlockIsTerminated(ilFn);
                size_t rhsEndBlockIdx = currentBlockIdx_;

                il::Type joinType = (lhsVal.type == il::Type::F64 && rhsVal.type == il::Type::F64) ? il::Type::F64 : il::Type::Dynamic;

                il::ValueId resParamId = ilFn.valueCount++;
                ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
                ExprJoin join = makeExprJoin(stateLhs, stateRhs, bJoin, ilFn);

                il::Instruction brInst;
                brInst.op = il::Op::Branch;
                brInst.type = il::Type::Void;
                brInst.result = il::kNoValue;
                brInst.operands = {lhsBool.id};

                // The skip edge's conversions (lhs result and join-var
                // coercions) feed the entry block's branch, so they must be
                // emitted there, not in the rhs block.
                setCurrentBlock(entryBlockIdx);
                Value lhsBoxed = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(lhsVal, ilFn) : unboxValueIfNeeded(lhsVal, joinType, ilFn);
                std::vector<il::ValueId> skipArgs{lhsBoxed.id};
                appendExprJoinArgs(skipArgs, join, stateLhs, ilFn);

                if (bin->op == ast::BinaryOp::LogicalAnd) {
                    brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                    brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                } else {
                    brInst.target = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                    brInst.elseTarget = il::BlockTarget{.block = bRhs, .args = {}};
                }
                ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

                if (rhsReaches) {
                    setCurrentBlock(rhsEndBlockIdx);
                    Value rhsConv = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(rhsVal, ilFn) : unboxValueIfNeeded(rhsVal, joinType, ilFn);
                    std::vector<il::ValueId> args{rhsConv.id};
                    appendExprJoinArgs(args, join, stateRhs, ilFn);
                    il::Instruction jmpInst;
                    jmpInst.op = il::Op::Jump;
                    jmpInst.type = il::Type::Void;
                    jmpInst.result = il::kNoValue;
                    jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                    emitInst(ilFn, jmpInst);
                }

                setCurrentBlock(bJoin);
                restoreVarStates(stateLhs);
                bindExprJoinParams(join);
                return Value{resParamId, joinType};
            }

            if (bin->op == ast::BinaryOp::NullishCoalescing) {
                auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
                if (!lhsOpt) return std::nullopt;
                Value lhsVal = *lhsOpt;

                if (lhsVal.type != il::Type::Dynamic) {
                    // A typed value is statically never nullish; the rhs is
                    // dead (and its side effects correctly never happen).
                    return lhsVal;
                }
                il::ValueId isNullishRes = ilFn.valueCount++;
                il::Instruction nullishInst;
                nullishInst.op = il::Op::IsNullish;
                nullishInst.type = il::Type::Bool;
                nullishInst.result = isNullishRes;
                nullishInst.operands = {lhsVal.id};
                emitInst(ilFn, nullishInst);

                il::BlockId bRhs = createBlock(ilFn);
                il::BlockId bJoin = createBlock(ilFn);
                size_t entryBlockIdx = currentBlockIdx_;
                auto stateLhs = snapshotVarStates();

                setCurrentBlock(bRhs);
                auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
                if (!rhsOpt) return std::nullopt;
                Value rhsVal = *rhsOpt;
                auto stateRhs = snapshotVarStates();
                bool rhsReaches = !currentBlockIsTerminated(ilFn);
                size_t rhsEndBlockIdx = currentBlockIdx_;

                il::Type joinType = il::Type::Dynamic;

                il::ValueId resParamId = ilFn.valueCount++;
                ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
                ExprJoin join = makeExprJoin(stateLhs, stateRhs, bJoin, ilFn);

                // Skip-edge coercions must dominate the entry branch.
                setCurrentBlock(entryBlockIdx);
                std::vector<il::ValueId> skipArgs{lhsVal.id};
                appendExprJoinArgs(skipArgs, join, stateLhs, ilFn);

                il::Instruction brInst;
                brInst.op = il::Op::Branch;
                brInst.type = il::Type::Void;
                brInst.result = il::kNoValue;
                brInst.operands = {isNullishRes};
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

                if (rhsReaches) {
                    setCurrentBlock(rhsEndBlockIdx);
                    Value rhsConv = boxValueIfNeeded(rhsVal, ilFn);
                    std::vector<il::ValueId> args{rhsConv.id};
                    appendExprJoinArgs(args, join, stateRhs, ilFn);
                    il::Instruction jmpInst;
                    jmpInst.op = il::Op::Jump;
                    jmpInst.type = il::Type::Void;
                    jmpInst.result = il::kNoValue;
                    jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                    emitInst(ilFn, jmpInst);
                }

                setCurrentBlock(bJoin);
                restoreVarStates(stateLhs);
                bindExprJoinParams(join);
                return Value{resParamId, joinType};
            }

            auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
            if (!lhsOpt) return std::nullopt;
            auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
            if (!rhsOpt) return std::nullopt;

            Value lhs = *lhsOpt;
            Value rhs = *rhsOpt;

            il::Op op;
            il::Type resType;

            switch (bin->op) {
                case ast::BinaryOp::Add:
                    if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic ||
                        lhs.type == il::Type::Str || rhs.type == il::Type::Str) {
                        auto lhsBoxed = boxValueIfNeeded(lhs, ilFn);
                        auto rhsBoxed = boxValueIfNeeded(rhs, ilFn);
                        il::ValueId res = ilFn.valueCount++;
                        il::Instruction inst;
                        inst.op = il::Op::Add;
                        inst.type = il::Type::Dynamic;
                        inst.result = res;
                        inst.operands = {lhsBoxed.id, rhsBoxed.id};
                        emitInst(ilFn, inst);
                        return Value{res, il::Type::Dynamic};
                    }
                    op = il::Op::Add;
                    resType = il::Type::F64;
                    break;
                case ast::BinaryOp::Sub:
                    op = il::Op::Sub;
                    resType = il::Type::F64;
                    break;
                case ast::BinaryOp::Mul:
                    op = il::Op::Mul;
                    resType = il::Type::F64;
                    break;
                case ast::BinaryOp::Div:
                    op = il::Op::Div;
                    resType = il::Type::F64;
                    break;
                case ast::BinaryOp::Mod:
                    op = il::Op::Mod;
                    resType = il::Type::F64;
                    break;
                case ast::BinaryOp::Less:
                    op = il::Op::CmpLt;
                    resType = il::Type::Bool;
                    break;
                case ast::BinaryOp::Greater:
                    op = il::Op::CmpGt;
                    resType = il::Type::Bool;
                    break;
                case ast::BinaryOp::LessEqual: {
                    // a <= b is !(a > b) -> cmp.gt, then cmp.eq false
                    Value l = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
                    Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
                    il::ValueId gtRes = ilFn.valueCount++;
                    il::Instruction gtInst;
                    gtInst.op = il::Op::CmpGt;
                    gtInst.type = il::Type::Bool;
                    gtInst.result = gtRes;
                    gtInst.operands = {l.id, r.id};
                    emitInst(ilFn, gtInst);

                    il::ValueId falseVal = ilFn.valueCount++;
                    il::Instruction falseInst;
                    falseInst.op = il::Op::ConstBool;
                    falseInst.type = il::Type::Bool;
                    falseInst.result = falseVal;
                    falseInst.immI32 = 0;
                    emitInst(ilFn, falseInst);

                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction cmpInst;
                    cmpInst.op = il::Op::CmpEq;
                    cmpInst.type = il::Type::Bool;
                    cmpInst.result = res;
                    cmpInst.operands = {gtRes, falseVal};
                    emitInst(ilFn, cmpInst);
                    return Value{res, il::Type::Bool};
                }
                case ast::BinaryOp::GreaterEqual: {
                    // a >= b is !(a < b)
                    Value l = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
                    Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
                    il::ValueId ltRes = ilFn.valueCount++;
                    il::Instruction ltInst;
                    ltInst.op = il::Op::CmpLt;
                    ltInst.type = il::Type::Bool;
                    ltInst.result = ltRes;
                    ltInst.operands = {l.id, r.id};
                    emitInst(ilFn, ltInst);

                    il::ValueId falseVal = ilFn.valueCount++;
                    il::Instruction falseInst;
                    falseInst.op = il::Op::ConstBool;
                    falseInst.type = il::Type::Bool;
                    falseInst.result = falseVal;
                    falseInst.immI32 = 0;
                    emitInst(ilFn, falseInst);

                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction cmpInst;
                    cmpInst.op = il::Op::CmpEq;
                    cmpInst.type = il::Type::Bool;
                    cmpInst.result = res;
                    cmpInst.operands = {ltRes, falseVal};
                    emitInst(ilFn, cmpInst);
                    return Value{res, il::Type::Bool};
                }
                case ast::BinaryOp::Eq:
                case ast::BinaryOp::StrictEq:
                case ast::BinaryOp::Ne:
                case ast::BinaryOp::StrictNe: {
                    bool negate = (bin->op == ast::BinaryOp::Ne || bin->op == ast::BinaryOp::StrictNe);
                    bool loose = (bin->op == ast::BinaryOp::Eq || bin->op == ast::BinaryOp::Ne);

                    Value eqRes{il::kNoValue, il::Type::Bool};
                    if (lhs.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic ||
                        lhs.type == il::Type::Str || rhs.type == il::Type::Str) {
                        if (loose) {
                            diags_.error(bin->span,
                                         "unsupported construct: loose equality (==, !=) on "
                                         "possibly non-numeric operands; use === / !==");
                            return std::nullopt;
                        }
                        Value lb = boxValueIfNeeded(lhs, ilFn);
                        Value rb = boxValueIfNeeded(rhs, ilFn);
                        il::ValueId res = ilFn.valueCount++;
                        il::Instruction inst;
                        inst.op = il::Op::StrictEq;
                        inst.type = il::Type::Bool;
                        inst.result = res;
                        inst.operands = {lb.id, rb.id};
                        emitInst(ilFn, inst);
                        eqRes = Value{res, il::Type::Bool};
                    } else if (lhs.type != rhs.type) {
                        if (loose) {
                            diags_.error(bin->span,
                                         "unsupported construct: loose equality (==, !=) on "
                                         "mixed primitive types; use === / !==");
                            return std::nullopt;
                        }
                        if (lhs.type == il::Type::I32 || rhs.type == il::Type::I32) {
                            diags_.error(bin->span,
                                         "unsupported construct: mixed i32/f64 strict equality");
                            return std::nullopt;
                        }
                        // Strict equality across distinct primitive types is
                        // statically false.
                        il::ValueId res = ilFn.valueCount++;
                        il::Instruction inst;
                        inst.op = il::Op::ConstBool;
                        inst.type = il::Type::Bool;
                        inst.result = res;
                        inst.immI32 = 0;
                        emitInst(ilFn, inst);
                        eqRes = Value{res, il::Type::Bool};
                    } else {
                        il::ValueId res = ilFn.valueCount++;
                        il::Instruction inst;
                        inst.op = negate ? il::Op::CmpNe : il::Op::CmpEq;
                        inst.type = il::Type::Bool;
                        inst.result = res;
                        inst.operands = {lhs.id, rhs.id};
                        emitInst(ilFn, inst);
                        eqRes = Value{res, il::Type::Bool};
                        negate = false;
                    }

                    if (negate) {
                        il::ValueId falseVal = ilFn.valueCount++;
                        il::Instruction falseInst;
                        falseInst.op = il::Op::ConstBool;
                        falseInst.type = il::Type::Bool;
                        falseInst.result = falseVal;
                        falseInst.immI32 = 0;
                        emitInst(ilFn, falseInst);

                        il::ValueId notRes = ilFn.valueCount++;
                        il::Instruction notInst;
                        notInst.op = il::Op::CmpEq;
                        notInst.type = il::Type::Bool;
                        notInst.result = notRes;
                        notInst.operands = {eqRes.id, falseVal};
                        emitInst(ilFn, notInst);
                        eqRes = Value{notRes, il::Type::Bool};
                    }
                    return eqRes;
                }
                default:
                    diags_.error(bin->span, "unsupported binary operator: " + std::string(ast::binaryOpName(bin->op)));
                    return std::nullopt;
            }

            // Arithmetic and relational comparison are numeric: dynamic
            // operands go through runtime-checked ToNumber first.
            if (resType == il::Type::F64 || op == il::Op::CmpLt || op == il::Op::CmpGt) {
                lhs = unboxValueIfNeeded(lhs, il::Type::F64, ilFn);
                rhs = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
            }

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = op;
            inst.type = resType;
            inst.result = res;
            inst.operands = {lhs.id, rhs.id};
            emitInst(ilFn, inst);
            return Value{res, resType};
        }

        if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
            bool isConsoleLog = false;
            if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
                if (calleeIdent->name == "console.log") isConsoleLog = true;
            } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
                if (const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
                    if (baseIdent->name == "console" && mem->property == "log") isConsoleLog = true;
                }
            }

            if (isConsoleLog) {
                if (call->args.size() != 1) {
                    diags_.error(call->span, "console.log expects 1 argument");
                    return std::nullopt;
                }
                auto argVal = lowerExpr(*call->args[0], ilFn);
                if (!argVal) return std::nullopt;

                auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
                il::Instruction inst;
                inst.op = il::Op::Print;
                inst.type = il::Type::Void;
                inst.result = il::kNoValue;
                inst.operands = {argBoxed.id};
                emitInst(ilFn, inst);
                return Value{il::kNoValue, il::Type::Void};
            }

            // `Object` is recognized here rather than looked up: bronze has
            // no global object for it to live on (docs/0009 decision 2).
            if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
                const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get());
                if (baseIdent && baseIdent->name == "Object" &&
                    activeVarMap_.find("Object") == activeVarMap_.end()) {
                    if (mem->property != "keys") {
                        diags_.error(call->span, "unsupported builtin: Object." + mem->property);
                        return std::nullopt;
                    }
                    if (call->args.size() != 1) {
                        diags_.error(call->span, "Object.keys expects 1 argument");
                        return std::nullopt;
                    }
                    auto argVal = lowerExpr(*call->args[0], ilFn);
                    if (!argVal) return std::nullopt;
                    auto argBoxed = boxValueIfNeeded(*argVal, ilFn);

                    il::ValueId res = ilFn.valueCount++;
                    il::Instruction inst;
                    inst.op = il::Op::ObjectKeys;
                    inst.type = il::Type::Dynamic;
                    inst.result = res;
                    inst.operands = {argBoxed.id};
                    emitInst(ilFn, inst);
                    return Value{res, il::Type::Dynamic};
                }
            }

            if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
                // A local binding shadows a module-level function, and so
                // does an enclosing scope's environment slot: a nested
                // function declaration registers in functionIndices_ under
                // its source name, but every reference to it — including a
                // sibling closure's — has to go through the environment
                // chain, not a direct call (docs/0007 decision 3).
                uint32_t shadowDepth = 0;
                uint32_t shadowIndex = 0;
                auto envIt = activeVarMap_.find(calleeIdent->name);
                if (envIt == activeVarMap_.end() &&
                    !findEnclosingEnvVar(calleeIdent->name, shadowDepth, shadowIndex)) {
                    auto it = functionIndices_.find(calleeIdent->name);
                    if (it != functionIndices_.end()) {
                        uint32_t calleeIdx = it->second;
                        const auto& calleeFn = ilModule_.functions[calleeIdx];

                        // Synthetic parameters are not source arguments;
                        // the arity the program has to match is the source
                        // one.
                        const size_t base = calleeFn.firstSourceParam();
                        if (call->args.size() != calleeFn.params.size() - base) {
                            diags_.error(call->span, "argument count mismatch in call to " + calleeIdent->name);
                            return std::nullopt;
                        }

                        std::vector<il::ValueId> argVals;
                        // A plain `f()` has no receiver, so a direct call
                        // supplies undefined for `__this` (docs/0008
                        // decision 3). `__env` cannot be supplied this way,
                        // which is why the verifier forbids direct calls to
                        // closures outright.
                        if (calleeFn.needsThis) {
                            argVals.push_back(emitConstUndefined(ilFn));
                        }
                        for (size_t i = 0; i < call->args.size(); ++i) {
                            auto argVal = lowerExpr(*call->args[i], ilFn);
                            if (!argVal) return std::nullopt;
                            const il::Type paramType = calleeFn.params[i + base].type;
                            if (paramType == il::Type::Dynamic) {
                                argVal = boxValueIfNeeded(*argVal, ilFn);
                            } else if (argVal->type == il::Type::Dynamic) {
                                argVal = unboxValueIfNeeded(*argVal, paramType, ilFn);
                            }
                            argVals.push_back(argVal->id);
                        }

                        il::Instruction inst;
                        inst.op = il::Op::Call;
                        inst.calleeIndex = calleeIdx;
                        inst.operands = std::move(argVals);
                        inst.type = calleeFn.returnType;

                        il::ValueId res = il::kNoValue;
                        if (calleeFn.returnType != il::Type::Void) {
                            res = ilFn.valueCount++;
                            inst.result = res;
                        } else {
                            inst.result = il::kNoValue;
                        }

                        emitInst(ilFn, inst);
                        return Value{res, calleeFn.returnType};
                    }
                }
            }

            Value calleeVal;
            Value thisArgVal;
            if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
                auto objVal = lowerExpr(*mem->object, ilFn);
                if (!objVal) return std::nullopt;
                thisArgVal = boxValueIfNeeded(*objVal, ilFn);

                uint32_t keyIdx = getKeyConstantIndex(mem->property);
                uint32_t icIdx = icSiteCounter_++;

                il::ValueId getRes = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::PropGet;
                inst.type = il::Type::Dynamic;
                inst.result = getRes;
                inst.operands = {thisArgVal.id};
                inst.keyIndex = keyIdx;
                inst.icIndex = icIdx;
                emitInst(ilFn, inst);
                calleeVal = Value{getRes, il::Type::Dynamic};
            } else {
                auto cVal = lowerExpr(*call->callee, ilFn);
                if (!cVal) return std::nullopt;
                calleeVal = boxValueIfNeeded(*cVal, ilFn);

                il::ValueId zeroRes = ilFn.valueCount++;
                il::Instruction zeroInst;
                zeroInst.op = il::Op::ConstF64;
                zeroInst.type = il::Type::F64;
                zeroInst.result = zeroRes;
                zeroInst.immF64 = 0.0;
                emitInst(ilFn, zeroInst);
                thisArgVal = boxValueIfNeeded(Value{zeroRes, il::Type::F64}, ilFn);
            }

            std::vector<il::ValueId> dynOperands;
            dynOperands.push_back(calleeVal.id);
            dynOperands.push_back(thisArgVal.id);

            for (const auto& argPtr : call->args) {
                auto argVal = lowerExpr(*argPtr, ilFn);
                if (!argVal) return std::nullopt;
                auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
                dynOperands.push_back(argBoxed.id);
            }

            il::ValueId callRes = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::DynamicCall;
            inst.type = il::Type::Dynamic;
            inst.result = callRes;
            inst.operands = std::move(dynOperands);
            emitInst(ilFn, inst);
            return Value{callRes, il::Type::Dynamic};
        }

        diags_.error(expr.span, "unsupported AST expression");
        return std::nullopt;
    }
};

}  // namespace

std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags) {
    Lowerer lowerer(astModule, diags);
    return lowerer.lower();
}

}  // namespace bronze::lower
