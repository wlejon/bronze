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
                    auto pType = mapTypeAnnotation(param.typeAnnotation, fnDecl->span, diags_);
                    if (!pType) return std::nullopt;
                    fn.params.push_back({param.name, *pType});
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
            if (!lowerStmtList(topLevelStmts, mainFn)) {
                return std::nullopt;
            }
            ilModule_.functions.push_back(std::move(mainFn));
        }

        if (diags_.hasErrors()) return std::nullopt;
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

    Value boxValueIfNeeded(Value val, il::Function& ilFn) {
        if (val.type == il::Type::Dynamic) return val;
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Box;
        inst.type = il::Type::Dynamic;
        inst.boxType = val.type;
        inst.result = res;
        inst.operands = {val.id};
        ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
            return Value{res, targetType};
        }
        return val;
    }

    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
        std::unordered_map<std::string, Value> env;
        for (uint32_t i = 0; i < fnDecl.params.size(); ++i) {
            env[fnDecl.params[i].name] = {i, ilFn.params[i].type};
        }
        std::vector<const ast::Stmt*> stmts;
        for (const auto& s : fnDecl.body) {
            stmts.push_back(s.get());
        }
        return lowerStmtList(stmts, ilFn, &env);
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
                } else if (ilFn.returnType == il::Type::Dynamic && val->type != il::Type::Dynamic) {
                    val = boxValueIfNeeded(*val, ilFn);
                } else if (ilFn.returnType != il::Type::Dynamic && val->type == il::Type::Dynamic) {
                    val = unboxValueIfNeeded(*val, ilFn.returnType, ilFn);
                }

                il::Instruction inst;
                inst.op = il::Op::Ret;
                inst.type = val->type;
                inst.result = il::kNoValue;
                inst.operands = {val->id};
                ilFn.body.push_back(inst);
            } else {
                il::Instruction inst;
                inst.op = il::Op::Ret;
                inst.type = il::Type::Void;
                inst.result = il::kNoValue;
                ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
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
                    ilFn.body.push_back(inst);
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

                    il::Instruction inst;
                    inst.op = il::Op::PropSet;
                    inst.type = il::Type::Void;
                    inst.result = il::kNoValue;
                    inst.operands = {objBoxed.id, rhsBoxed.id};
                    inst.keyIndex = keyIdx;
                    inst.icIndex = icIdx;
                    ilFn.body.push_back(inst);
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
            ilFn.body.push_back(inst);
            return Value{res, resType};
        }

        if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
            if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
                if (calleeIdent->name == "console.log") {
                    if (call->args.size() != 1) {
                        diags_.error(call->span, "console.log expects 1 argument");
                        return std::nullopt;
                    }
                    auto argVal = lowerExpr(*call->args[0], ilFn, env);
                    if (!argVal) return std::nullopt;

                    if (ilFn.returnType == il::Type::Void) {
                        ilFn.returnType = argVal->type;
                    }
                    il::Instruction inst;
                    inst.op = il::Op::Ret;
                    inst.type = argVal->type;
                    inst.result = il::kNoValue;
                    inst.operands = {argVal->id};
                    ilFn.body.push_back(inst);
                    return argVal;
                }

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

                    ilFn.body.push_back(inst);
                    return Value{res, calleeFn.returnType};
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
                ilFn.body.push_back(inst);
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
                ilFn.body.push_back(zeroInst);
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
            ilFn.body.push_back(inst);
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
