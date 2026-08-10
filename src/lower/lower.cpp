#include "lower/lower.h"

#include <unordered_map>

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
            if (!lowerStmtList(topLevelStmts, mainFn)) {
                return std::nullopt;
            }
            auto& insts = mainFn.blocks.back().instructions;
            if (insts.empty() || !il::isTerminator(insts.back().op)) {
                il::Instruction retInst;
                retInst.op = il::Op::Ret;
                retInst.type = il::Type::Void;
                insts.push_back(retInst);
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

    uint32_t getKeyConstantIndex(const std::string& key) {
        auto it = keyConstants_.find(key);
        if (it != keyConstants_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(keyConstants_.size());
        keyConstants_[key] = idx;
        return idx;
    }

    void emitInst(il::Function& ilFn, const il::Instruction& inst) {
        if (ilFn.blocks.empty()) {
            ilFn.blocks.push_back(il::Block{.id = 0});
        }
        ilFn.blocks.back().instructions.push_back(inst);
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
        return val;
    }

    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
        ilFn.blocks.push_back(il::Block{.id = 0});
        std::unordered_map<std::string, Value> env;
        for (uint32_t i = 0; i < fnDecl.params.size(); ++i) {
            env[fnDecl.params[i].name] = {i, ilFn.params[i].type};
        }
        std::vector<const ast::Stmt*> stmts;
        for (const auto& s : fnDecl.body) {
            stmts.push_back(s.get());
        }
        if (!lowerStmtList(stmts, ilFn, &env)) return false;
        auto& insts = ilFn.blocks.back().instructions;
        if (insts.empty() || !il::isTerminator(insts.back().op)) {
            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            retInst.type = ilFn.returnType;
            insts.push_back(retInst);
        }
        return true;
    }

    bool lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn,
                       std::unordered_map<std::string, Value>* parentEnv = nullptr) {
        std::unordered_map<std::string, Value> env;
        if (parentEnv) env = *parentEnv;

        for (const auto* stmt : stmts) {
            if (!lowerStmt(*stmt, ilFn, env)) {
                return false;
            }
        }
        return true;
    }

    bool lowerStmt(const ast::Stmt& stmt, il::Function& ilFn, std::unordered_map<std::string, Value>& env) {
        if (const auto* varDecl = dynamic_cast<const ast::VarDecl*>(&stmt)) {
            if (!varDecl->init) {
                diags_.error(varDecl->span, "variable declaration missing initializer");
                return false;
            }
            auto initVal = lowerExpr(*varDecl->init, ilFn, env);
            if (!initVal) return false;

            if (!varDecl->typeAnnotation.empty()) {
                auto annType = mapTypeAnnotation(varDecl->typeAnnotation, varDecl->span, diags_);
                if (!annType) return false;
                if (*annType == il::Type::Dynamic && initVal->type != il::Type::Dynamic) {
                    initVal = boxValueIfNeeded(*initVal, ilFn);
                } else if (*annType != il::Type::Dynamic && initVal->type == il::Type::Dynamic) {
                    initVal = unboxValueIfNeeded(*initVal, *annType, ilFn);
                }
            }
            env[varDecl->name] = *initVal;
            return true;
        }

        if (const auto* retStmt = dynamic_cast<const ast::ReturnStmt*>(&stmt)) {
            if (retStmt->value) {
                auto val = lowerExpr(*retStmt->value, ilFn, env);
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
            auto val = lowerExpr(*exprStmt->expr, ilFn, env);
            if (!val) return false;
            return true;
        }

        if (const auto* ifStmt = dynamic_cast<const ast::IfStmt*>(&stmt)) {
            diags_.error(ifStmt->span, "unsupported AST node: IfStmt");
            return false;
        }

        diags_.error(stmt.span, "unsupported AST node");
        return false;
    }

    std::optional<Value> lowerExpr(const ast::Expr& expr, il::Function& ilFn,
                                   std::unordered_map<std::string, Value>& env) {
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
                auto valOpt = lowerExpr(*prop.value, ilFn, env);
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
                auto elemOpt = lowerExpr(*arrLit->elements[i], ilFn, env);
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
            std::string fnName = fnExpr->name;
            if (fnName.empty()) {
                fnName = "__anon_fn_" + std::to_string(ilModule_.functions.size());
            }
            il::Function newFn;
            newFn.name = fnName;
            newFn.returnType = il::Type::Dynamic;
            for (const auto& param : fnExpr->params) {
                if (!param.typeAnnotation.empty()) {
                    auto pType = mapTypeAnnotation(param.typeAnnotation, fnExpr->span, diags_);
                    if (!pType) return std::nullopt;
                    newFn.params.push_back({param.name, *pType});
                } else {
                    newFn.params.push_back({param.name, il::Type::Dynamic});
                }
            }
            if (!fnExpr->returnType.empty()) {
                auto rType = mapTypeAnnotation(fnExpr->returnType, fnExpr->span, diags_);
                if (!rType) return std::nullopt;
                newFn.returnType = *rType;
            }
            newFn.valueCount = static_cast<uint32_t>(newFn.params.size());

            std::unordered_map<std::string, Value> fnEnv = env;
            for (uint32_t i = 0; i < fnExpr->params.size(); ++i) {
                fnEnv[fnExpr->params[i].name] = {i, newFn.params[i].type};
            }
            std::vector<const ast::Stmt*> stmts;
            for (const auto& s : fnExpr->body) stmts.push_back(s.get());
            if (!lowerStmtList(stmts, newFn, &fnEnv)) {
                return std::nullopt;
            }
            bool hasRet = false;
            if (!newFn.blocks.empty()) {
                for (const auto& inst : newFn.blocks[0].instructions) {
                    if (inst.op == il::Op::Ret) { hasRet = true; break; }
                }
            }
            if (!hasRet) {
                il::Instruction retInst;
                retInst.op = il::Op::Ret;
                retInst.type = il::Type::Void;
                retInst.result = il::kNoValue;
                emitInst(newFn, retInst);
            }

            uint32_t createdFnIdx = static_cast<uint32_t>(ilModule_.functions.size());
            functionIndices_[fnName] = createdFnIdx;
            ilModule_.functions.push_back(std::move(newFn));

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::CreateFunction;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.calleeIndex = createdFnIdx;
            inst.immI32 = static_cast<int32_t>(fnExpr->params.size());
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }

        if (const auto* ident = dynamic_cast<const ast::Ident*>(&expr)) {
            auto it = env.find(ident->name);
            if (it == env.end()) {
                diags_.error(ident->span, "undefined variable: " + ident->name);
                return std::nullopt;
            }
            return it->second;
        }

        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&expr)) {
            auto objVal = lowerExpr(*mem->object, ilFn, env);
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
            auto objVal = lowerExpr(*idxAccess->object, ilFn, env);
            if (!objVal) return std::nullopt;
            auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

            uint32_t keyIdx = 0;
            if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(idxAccess->index.get())) {
                keyIdx = getKeyConstantIndex(std::to_string(static_cast<int64_t>(numLit->value)));
            } else if (const auto* strLit = dynamic_cast<const ast::StringLit*>(idxAccess->index.get())) {
                keyIdx = getKeyConstantIndex(strLit->value);
            } else {
                auto indexVal = lowerExpr(*idxAccess->index, ilFn, env);
                if (!indexVal) return std::nullopt;
                keyIdx = 0;
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
            if (bin->op == ast::BinaryOp::Assign) {
                if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
                    auto objVal = lowerExpr(*mem->object, ilFn, env);
                    if (!objVal) return std::nullopt;
                    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
                    auto rhsVal = lowerExpr(*bin->rhs, ilFn, env);
                    if (!rhsVal) return std::nullopt;
                    auto rhsBoxed = boxValueIfNeeded(*rhsVal, ilFn);

                    uint32_t keyIdx = getKeyConstantIndex(mem->property);
                    uint32_t icIdx = icSiteCounter_++;

                    il::Instruction inst;
                    inst.op = il::Op::PropSet;
                    inst.type = il::Type::Void;
                    inst.result = il::kNoValue;
                    inst.operands = {objBoxed.id, rhsBoxed.id};
                    inst.keyIndex = keyIdx;
                    inst.icIndex = icIdx;
                    emitInst(ilFn, inst);
                    return rhsBoxed;
                }
                if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(bin->lhs.get())) {
                    auto objVal = lowerExpr(*idxAccess->object, ilFn, env);
                    if (!objVal) return std::nullopt;
                    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
                    auto rhsVal = lowerExpr(*bin->rhs, ilFn, env);
                    if (!rhsVal) return std::nullopt;
                    auto rhsBoxed = boxValueIfNeeded(*rhsVal, ilFn);

                    uint32_t keyIdx = 0;
                    if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(idxAccess->index.get())) {
                        keyIdx = getKeyConstantIndex(std::to_string(static_cast<int64_t>(numLit->value)));
                    } else if (const auto* strLit = dynamic_cast<const ast::StringLit*>(idxAccess->index.get())) {
                        keyIdx = getKeyConstantIndex(strLit->value);
                    } else {
                        auto indexVal = lowerExpr(*idxAccess->index, ilFn, env);
                        if (!indexVal) return std::nullopt;
                        keyIdx = 0;
                    }

                    uint32_t icIdx = icSiteCounter_++;

                    il::Instruction setInst;
                    setInst.op = il::Op::PropSet;
                    setInst.type = il::Type::Void;
                    setInst.result = il::kNoValue;
                    setInst.operands = {objBoxed.id, rhsBoxed.id};
                    setInst.keyIndex = keyIdx;
                    setInst.icIndex = icIdx;
                    emitInst(ilFn, setInst);
                    return rhsBoxed;
                }
                if (const auto* ident = dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
                    auto rhsVal = lowerExpr(*bin->rhs, ilFn, env);
                    if (!rhsVal) return std::nullopt;
                    env[ident->name] = *rhsVal;
                    return rhsVal;
                }
                diags_.error(bin->span, "invalid assignment target");
                return std::nullopt;
            }

            auto lhsOpt = lowerExpr(*bin->lhs, ilFn, env);
            if (!lhsOpt) return std::nullopt;
            auto rhsOpt = lowerExpr(*bin->rhs, ilFn, env);
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
                case ast::BinaryOp::Less:
                    op = il::Op::CmpLt;
                    resType = il::Type::Bool;
                    break;
                case ast::BinaryOp::Greater:
                    op = il::Op::CmpGt;
                    resType = il::Type::Bool;
                    break;
                case ast::BinaryOp::Eq:
                case ast::BinaryOp::StrictEq:
                    op = il::Op::CmpEq;
                    resType = il::Type::Bool;
                    break;
                default:
                    diags_.error(bin->span, "unsupported binary operator: " + std::string(ast::binaryOpName(bin->op)));
                    return std::nullopt;
            }

            if (resType == il::Type::F64) {
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
                auto argVal = lowerExpr(*call->args[0], ilFn, env);
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

            if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
                auto envIt = env.find(calleeIdent->name);
                if (envIt == env.end()) {
                    auto it = functionIndices_.find(calleeIdent->name);
                    if (it != functionIndices_.end()) {
                        uint32_t calleeIdx = it->second;
                        const auto& calleeFn = ilModule_.functions[calleeIdx];

                        if (call->args.size() != calleeFn.params.size()) {
                            diags_.error(call->span, "argument count mismatch in call to " + calleeIdent->name);
                            return std::nullopt;
                        }

                        std::vector<il::ValueId> argVals;
                        for (size_t i = 0; i < call->args.size(); ++i) {
                            auto argVal = lowerExpr(*call->args[i], ilFn, env);
                            if (!argVal) return std::nullopt;
                            if (calleeFn.params[i].type == il::Type::Dynamic) {
                                argVal = boxValueIfNeeded(*argVal, ilFn);
                            } else if (argVal->type == il::Type::Dynamic) {
                                argVal = unboxValueIfNeeded(*argVal, calleeFn.params[i].type, ilFn);
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
                auto objVal = lowerExpr(*mem->object, ilFn, env);
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
                auto cVal = lowerExpr(*call->callee, ilFn, env);
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
                auto argVal = lowerExpr(*argPtr, ilFn, env);
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
