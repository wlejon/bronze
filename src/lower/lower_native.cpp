#include <string>
#include <vector>

#include "lower/lowerer.h"
#include "lower/native_manifest.h"

namespace bronze::lower {

std::string Lowerer::getDottedPath(const ast::Expr* expr) const {
    if (const auto* id = dynamic_cast<const ast::Ident*>(expr)) {
        return id->name;
    }
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(expr)) {
        if (mem->isPrivate) return "";
        std::string base = getDottedPath(mem->object.get());
        if (base.empty()) return "";
        return base + "." + mem->property;
    }
    return "";
}

void Lowerer::initNativeManifestGlobals() {
    if (!nativeManifest_) return;
    for (const auto& root : nativeManifest_->namespaceRoots()) {
        hostGlobals_.insert(root);
    }
    for (const auto& cls : nativeManifest_->knownClasses()) {
        hostGlobals_.insert(cls);
    }
}

uint32_t Lowerer::registerExternalFunction(const std::string& symbol, il::Type returnType,
                                          const std::vector<il::Type>& paramTypes) {
    auto it = functionIndices_.find(symbol);
    if (it != functionIndices_.end()) {
        return it->second;
    }
    uint32_t idx = static_cast<uint32_t>(ilModule_.functions.size());
    il::Function fn;
    fn.name = symbol;
    fn.returnType = returnType;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        il::Param p;
        p.name = "arg" + std::to_string(i);
        p.type = paramTypes[i];
        fn.params.push_back(p);
    }
    // blocks remains empty to mark an external C-ABI symbol declaration
    ilModule_.functions.push_back(std::move(fn));
    functionIndices_[symbol] = idx;
    return idx;
}

Lowerer::Value Lowerer::emitDefaultValueForType(il::Type type, il::Function& ilFn) {
    if (type == il::Type::Str) {
        uint32_t keyIdx = getKeyConstantIndex("");
        il::ValueId boxId = ilFn.valueCount++;
        il::Instruction boxInst;
        boxInst.op = il::Op::Box;
        boxInst.type = il::Type::Dynamic;
        boxInst.boxType = il::Type::Str;
        boxInst.result = boxId;
        boxInst.keyIndex = keyIdx;
        emitInst(ilFn, boxInst);
        return unboxValueIfNeeded(Value{boxId, il::Type::Dynamic}, il::Type::Str, ilFn);
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.result = res;
    inst.type = type;
    switch (type) {
        case il::Type::F64:
            inst.op = il::Op::ConstF64;
            inst.immF64 = 0.0;
            break;
        case il::Type::I32:
            inst.op = il::Op::ConstI32;
            inst.immI32 = 0;
            break;
        case il::Type::Bool:
            inst.op = il::Op::ConstBool;
            inst.immI32 = 0;
            break;
        default:
            inst.op = il::Op::ConstUndefined;
            inst.type = il::Type::Dynamic;
            break;
    }
    emitInst(ilFn, inst);
    return Value{res, inst.type};
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeCall(const ast::Call* call, il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;

    // 1. Direct namespace/static functions, e.g. bro.math.lerp(...)
    std::string dotted = getDottedPath(call->callee.get());
    if (!dotted.empty()) {
        const auto* sig = nativeManifest_->findFunction(dotted);
        if (sig) {
            const auto expectedTypes = sig->toIlParamTypes();
            std::vector<il::ValueId> argValIds;
            for (size_t i = 0; i < sig->paramTypes.size(); ++i) {
                il::Type expType = expectedTypes[i];
                Value coerced{il::kNoValue, expType};
                if (i < call->args.size()) {
                    auto argVal = lowerExpr(*call->args[i], ilFn);
                    if (!argVal) return std::nullopt;

                    coerced = *argVal;
                    if (expType == il::Type::F64) {
                        coerced = unboxValueIfNeeded(coerced, il::Type::F64, ilFn);
                    } else if (expType == il::Type::I32) {
                        coerced = emitToInt32(coerced, ilFn);
                    } else if (expType == il::Type::Bool) {
                        coerced = unboxValueIfNeeded(coerced, il::Type::Bool, ilFn);
                    } else if (expType == il::Type::Str) {
                        coerced = unboxValueIfNeeded(coerced, il::Type::Str, ilFn);
                    } else {
                        coerced = boxValueIfNeeded(coerced, ilFn);
                    }
                } else {
                    coerced = emitDefaultValueForType(expType, ilFn);
                }
                argValIds.push_back(coerced.id);
            }

            // Evaluate trailing arguments for side-effects
            for (size_t i = sig->paramTypes.size(); i < call->args.size(); ++i) {
                if (!lowerExpr(*call->args[i], ilFn)) return std::nullopt;
            }

            uint32_t calleeIdx = registerExternalFunction(sig->symbol, sig->toIlReturnType(), expectedTypes);
            il::Type retType = sig->toIlReturnType();
            il::ValueId res = (retType != il::Type::Void) ? ilFn.valueCount++ : il::kNoValue;

            il::Instruction inst;
            inst.op = il::Op::Call;
            inst.type = retType;
            inst.result = res;
            inst.operands = std::move(argValIds);
            inst.calleeIndex = calleeIdx;
            emitInst(ilFn, inst);

            if (retType == il::Type::Void) {
                return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            }
            if (retType == il::Type::Str) {
                return boxValueIfNeeded(Value{res, il::Type::Str}, ilFn);
            }
            return Value{res, retType, sig->returnClass};
        }
    }

    // 2. Method invocation on a native class instance, e.g. sh.insert(...)
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        std::string className;
        if (const auto* objIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
            auto it = varNativeClasses_.find(objIdent->name);
            if (it != varNativeClasses_.end()) {
                className = it->second;
            }
        }

        if (!className.empty()) {
            const auto* cls = nativeManifest_->findClass(className);
            if (cls) {
                auto mIt = cls->methods.find(mem->property);
                if (mIt != cls->methods.end()) {
                    const auto& msig = mIt->second;
                    // First parameter of native method C-ABI is the 'self' handle (dynamic)
                    auto objVal = lowerExpr(*mem->object, ilFn);
                    if (!objVal) return std::nullopt;

                    const auto expectedTypes = msig.toIlParamTypes();
                    std::vector<il::ValueId> argValIds;
                    // self argument
                    argValIds.push_back(boxValueIfNeeded(*objVal, ilFn).id);

                    // Method source arguments start from index 1 in expectedTypes
                    size_t methodArgCount = expectedTypes.size() > 0 ? expectedTypes.size() - 1 : 0;
                    for (size_t i = 0; i < methodArgCount; ++i) {
                        il::Type expType = expectedTypes[i + 1];
                        Value coerced{il::kNoValue, expType};
                        if (i < call->args.size()) {
                            auto argVal = lowerExpr(*call->args[i], ilFn);
                            if (!argVal) return std::nullopt;

                            coerced = *argVal;
                            if (expType == il::Type::F64) {
                                coerced = unboxValueIfNeeded(coerced, il::Type::F64, ilFn);
                            } else if (expType == il::Type::I32) {
                                coerced = emitToInt32(coerced, ilFn);
                            } else if (expType == il::Type::Bool) {
                                coerced = unboxValueIfNeeded(coerced, il::Type::Bool, ilFn);
                            } else if (expType == il::Type::Str) {
                                coerced = unboxValueIfNeeded(coerced, il::Type::Str, ilFn);
                            } else {
                                coerced = boxValueIfNeeded(coerced, ilFn);
                            }
                        } else {
                            coerced = emitDefaultValueForType(expType, ilFn);
                        }
                        argValIds.push_back(coerced.id);
                    }

                    // Trailing extra arguments
                    for (size_t i = methodArgCount; i < call->args.size(); ++i) {
                        if (!lowerExpr(*call->args[i], ilFn)) return std::nullopt;
                    }

                    uint32_t calleeIdx = registerExternalFunction(msig.symbol, msig.toIlReturnType(), expectedTypes);
                    il::Type retType = msig.toIlReturnType();
                    il::ValueId res = (retType != il::Type::Void) ? ilFn.valueCount++ : il::kNoValue;

                    il::Instruction inst;
                    inst.op = il::Op::Call;
                    inst.type = retType;
                    inst.result = res;
                    inst.operands = std::move(argValIds);
                    inst.calleeIndex = calleeIdx;
                    emitInst(ilFn, inst);

                    if (retType == il::Type::Void) {
                        return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
                    }
                    if (retType == il::Type::Str) {
                        return boxValueIfNeeded(Value{res, il::Type::Str}, ilFn);
                    }
                    return Value{res, retType, msig.returnClass};
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeNew(const ast::NewExpr* newExpr, il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;

    std::string className;
    if (const auto* id = dynamic_cast<const ast::Ident*>(newExpr->callee.get())) {
        className = id->name;
    } else {
        className = getDottedPath(newExpr->callee.get());
    }

    if (className.empty()) return std::nullopt;

    const auto* cls = nativeManifest_->findClass(className);
    if (!cls || cls->constructor.symbol.empty()) return std::nullopt;

    const auto& ctor = cls->constructor;
    const auto expectedTypes = ctor.toIlParamTypes();

    std::vector<il::ValueId> argValIds;
    for (size_t i = 0; i < expectedTypes.size(); ++i) {
        il::Type expType = expectedTypes[i];
        Value coerced{il::kNoValue, expType};
        if (i < newExpr->args.size()) {
            auto argVal = lowerExpr(*newExpr->args[i], ilFn);
            if (!argVal) return std::nullopt;

            coerced = *argVal;
            if (expType == il::Type::F64) {
                coerced = unboxValueIfNeeded(coerced, il::Type::F64, ilFn);
            } else if (expType == il::Type::I32) {
                coerced = emitToInt32(coerced, ilFn);
            } else if (expType == il::Type::Bool) {
                coerced = unboxValueIfNeeded(coerced, il::Type::Bool, ilFn);
            } else if (expType == il::Type::Str) {
                coerced = unboxValueIfNeeded(coerced, il::Type::Str, ilFn);
            } else {
                coerced = boxValueIfNeeded(coerced, ilFn);
            }
        } else {
            coerced = emitDefaultValueForType(expType, ilFn);
        }
        argValIds.push_back(coerced.id);
    }

    for (size_t i = expectedTypes.size(); i < newExpr->args.size(); ++i) {
        if (!lowerExpr(*newExpr->args[i], ilFn)) return std::nullopt;
    }

    uint32_t calleeIdx = registerExternalFunction(ctor.symbol, il::Type::Dynamic, expectedTypes);
    il::ValueId res = ilFn.valueCount++;

    il::Instruction inst;
    inst.op = il::Op::Call;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(argValIds);
    inst.calleeIndex = calleeIdx;
    emitInst(ilFn, inst);

    return Value{res, il::Type::Dynamic, className};
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativePropertyGet(const ast::MemberAccess* mem,
                                                                il::Function& ilFn, bool onSpine) {
    if (!nativeManifest_ || mem->isPrivate) return std::nullopt;

    // 1. Namespace property access, e.g. bro.time.scale, bro.time.now
    std::string dotted = getDottedPath(mem);
    if (!dotted.empty()) {
        const auto* psig = nativeManifest_->findNamespaceProperty(dotted);
        if (psig && !psig->getterSymbol.empty()) {
            il::Type retType = nativeTypeToIl(psig->type);
            uint32_t calleeIdx = registerExternalFunction(psig->getterSymbol, retType, {});
            il::ValueId res = (retType != il::Type::Void) ? ilFn.valueCount++ : il::kNoValue;
            il::Instruction inst;
            inst.op = il::Op::Call;
            inst.type = retType;
            inst.result = res;
            inst.operands = {};
            inst.calleeIndex = calleeIdx;
            emitInst(ilFn, inst);
            if (retType == il::Type::Void) {
                return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            }
            if (retType == il::Type::Str) {
                return boxValueIfNeeded(Value{res, il::Type::Str}, ilFn);
            }
            return Value{res, retType};
        }
    }

    // 2. Class instance property access, e.g. smoother.current
    std::string className;
    if (const auto* objIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
        auto it = varNativeClasses_.find(objIdent->name);
        if (it != varNativeClasses_.end()) {
            className = it->second;
        }
    }
    if (!className.empty()) {
        const auto* cls = nativeManifest_->findClass(className);
        if (cls) {
            auto pIt = cls->properties.find(mem->property);
            if (pIt != cls->properties.end() && !pIt->second.getterSymbol.empty()) {
                auto objVal = lowerChainBase(*mem->object, ilFn, onSpine);
                if (!objVal) return std::nullopt;
                il::Type retType = nativeTypeToIl(pIt->second.type);
                uint32_t calleeIdx = registerExternalFunction(pIt->second.getterSymbol, retType, {il::Type::Dynamic});
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::Call;
                inst.type = retType;
                inst.result = res;
                inst.operands = {boxValueIfNeeded(*objVal, ilFn).id};
                inst.calleeIndex = calleeIdx;
                emitInst(ilFn, inst);
                if (retType == il::Type::Str) {
                    return boxValueIfNeeded(Value{res, il::Type::Str}, ilFn);
                }
                return Value{res, retType};
            }
        }
    }

    return std::nullopt;
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeAssignment(const ast::Binary* bin,
                                                               il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;

    const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get());
    if (!mem || mem->isPrivate) return std::nullopt;

    // 1. Namespace property assignment, e.g. bro.time.scale = 2.0
    std::string dotted = getDottedPath(mem);
    if (!dotted.empty()) {
        const auto* psig = nativeManifest_->findNamespaceProperty(dotted);
        if (psig) {
            if (psig->setterSymbol.empty()) {
                diags_.error(bin->lhs->span, "Cannot assign to read-only property '" + dotted + "'");
                return std::nullopt;
            }

            il::Type propType = nativeTypeToIl(psig->type);

            std::optional<Value> curVal;
            if (bin->op != ast::BinaryOp::Assign) {
                if (psig->getterSymbol.empty()) {
                    diags_.error(bin->lhs->span, "Cannot read property for compound assignment '" + dotted + "'");
                    return std::nullopt;
                }
                uint32_t getIdx = registerExternalFunction(psig->getterSymbol, propType, {});
                il::ValueId curId = ilFn.valueCount++;
                il::Instruction getInst;
                getInst.op = il::Op::Call;
                getInst.type = propType;
                getInst.result = curId;
                getInst.operands = {};
                getInst.calleeIndex = getIdx;
                emitInst(ilFn, getInst);
                curVal = Value{curId, propType};
            }

            auto rhsVal = lowerExpr(*bin->rhs, ilFn);
            if (!rhsVal) return std::nullopt;

            Value combined = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op, provenNumber(*bin), ilFn)
                                    : *rhsVal;

            Value coerced = combined;
            if (propType == il::Type::F64) {
                coerced = unboxValueIfNeeded(coerced, il::Type::F64, ilFn);
            } else if (propType == il::Type::I32) {
                coerced = emitToInt32(coerced, ilFn);
            } else if (propType == il::Type::Bool) {
                coerced = unboxValueIfNeeded(coerced, il::Type::Bool, ilFn);
            } else if (propType == il::Type::Str) {
                coerced = unboxValueIfNeeded(coerced, il::Type::Str, ilFn);
            } else {
                coerced = boxValueIfNeeded(coerced, ilFn);
            }

            uint32_t setIdx = registerExternalFunction(psig->setterSymbol, il::Type::Void, {propType});
            il::Instruction setInst;
            setInst.op = il::Op::Call;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {coerced.id};
            setInst.calleeIndex = setIdx;
            emitInst(ilFn, setInst);

            return boxValueIfNeeded(coerced, ilFn);
        }
    }

    // 2. Class instance property assignment, e.g. obj.prop = val
    std::string className;
    if (const auto* objIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
        auto it = varNativeClasses_.find(objIdent->name);
        if (it != varNativeClasses_.end()) {
            className = it->second;
        }
    }
    if (!className.empty()) {
        const auto* cls = nativeManifest_->findClass(className);
        if (cls) {
            auto pIt = cls->properties.find(mem->property);
            if (pIt != cls->properties.end()) {
                const auto& prop = pIt->second;
                if (prop.setterSymbol.empty()) {
                    diags_.error(bin->lhs->span, "Cannot assign to read-only property '" + mem->property + "' on class '" + className + "'");
                    return std::nullopt;
                }

                auto objVal = lowerExpr(*mem->object, ilFn);
                if (!objVal) return std::nullopt;
                auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

                il::Type propType = nativeTypeToIl(prop.type);

                std::optional<Value> curVal;
                if (bin->op != ast::BinaryOp::Assign) {
                    if (prop.getterSymbol.empty()) {
                        diags_.error(bin->lhs->span, "Cannot read property for compound assignment '" + mem->property + "'");
                        return std::nullopt;
                    }
                    uint32_t getIdx = registerExternalFunction(prop.getterSymbol, propType, {il::Type::Dynamic});
                    il::ValueId curId = ilFn.valueCount++;
                    il::Instruction getInst;
                    getInst.op = il::Op::Call;
                    getInst.type = propType;
                    getInst.result = curId;
                    getInst.operands = {objBoxed.id};
                    getInst.calleeIndex = getIdx;
                    emitInst(ilFn, getInst);
                    curVal = Value{curId, propType};
                }

                auto rhsVal = lowerExpr(*bin->rhs, ilFn);
                if (!rhsVal) return std::nullopt;

                Value combined = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op, provenNumber(*bin), ilFn)
                                        : *rhsVal;

                Value coerced = combined;
                if (propType == il::Type::F64) {
                    coerced = unboxValueIfNeeded(coerced, il::Type::F64, ilFn);
                } else if (propType == il::Type::I32) {
                    coerced = emitToInt32(coerced, ilFn);
                } else if (propType == il::Type::Bool) {
                    coerced = unboxValueIfNeeded(coerced, il::Type::Bool, ilFn);
                } else if (propType == il::Type::Str) {
                    coerced = unboxValueIfNeeded(coerced, il::Type::Str, ilFn);
                } else {
                    coerced = boxValueIfNeeded(coerced, ilFn);
                }

                uint32_t setIdx = registerExternalFunction(prop.setterSymbol, il::Type::Void, {il::Type::Dynamic, propType});
                il::Instruction setInst;
                setInst.op = il::Op::Call;
                setInst.type = il::Type::Void;
                setInst.result = il::kNoValue;
                setInst.operands = {objBoxed.id, coerced.id};
                setInst.calleeIndex = setIdx;
                emitInst(ilFn, setInst);

                return boxValueIfNeeded(coerced, ilFn);
            }
        }
    }

    return std::nullopt;
}

}  // namespace bronze::lower
