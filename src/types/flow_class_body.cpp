// Nested functions, object literals, constructor shapes and class body analysis
// for flow analysis. Decomposed from flow_expr.cpp to keep both files strictly
// under the line limit.

#include <algorithm>
#include <string>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "ast/query_walk.h"
#include "types/flow_analyzer.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

// The ordered property names a constructor installs on `this`, in source order.
// Does not descend into nested functions: each one binds its own receiver, so
// an inner `this.x =` says nothing about this constructor.
class ThisPropertyWalker final : public Walker {
public:
    std::vector<std::string> properties;

    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::FunctionDecl&) override {}

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* member = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (!member->isPrivate &&
                    dynamic_cast<const ast::ThisExpr*>(member->object.get())) {
                    if (std::find(properties.begin(), properties.end(), member->property) ==
                        properties.end()) {
                        properties.push_back(member->property);
                    }
                }
            }
        }
        Walker::visit(n);
    }
};

}  // namespace

ShapeClassId FlowAnalyzer::constructorShape(const std::string& name) {
    if (const ClassLayout* cl = mod_.result->classLayouts.byName(name)) {
        return cl->shapeClass;
    }
    const auto found = mod_.indexByName.find(name);
    if (found == mod_.indexByName.end()) return kNoShapeClass;
    const uint32_t index = found->second;
    mod_.contribute(InferMemo::Contrib::FnReached, index, 0, Type::never());
    if (const auto it = mod_.ctorShapes.find(index); it != mod_.ctorShapes.end()) {
        return it->second;
    }
    ThisPropertyWalker walker;
    walker.walkList(mod_.functions[index].decl->body);
    const ShapeClassId cls = mod_.result->shapes.intern(name, std::move(walker.properties));
    mod_.ctorShapes.emplace(index, cls);
    return cls;
}

Type FlowAnalyzer::objectLit(const ast::ObjectLit& o) {
    std::vector<std::string> props;
    std::vector<std::pair<std::string, Type>> propTypes;
    bool computedKey = false;
    bool unclosed = false;
    for (const auto& p : o.props) {
        const bool isSpread = dynamic_cast<const ast::SpreadElement*>(p.value.get()) != nullptr;
        if (isSpread || p.accessor != ast::AccessorKind::None) unclosed = true;
        if (p.keyExpr) {
            computedKey = true;
            expr(*p.keyExpr);
        }
        const bool skipNsGetter = o.isModuleNamespace && !record_ && !mod_.auditSettled &&
                                  mod_.valueFlow && p.accessor == ast::AccessorKind::Getter;
        const bool suppress = scope_.moduleTopLevel && !p.keyExpr && !isSpread &&
                              p.accessor == ast::AccessorKind::None &&
                              ast::exactAs<const ast::Ident*>(p.value.get()) != nullptr;
        const bool savedSuppress = suppressFnReach_;
        if (suppress) suppressFnReach_ = true;
        const Type valT = skipNsGetter ? Type::dynamic() : expr(*p.value);
        suppressFnReach_ = savedSuppress;
        if (p.keyExpr) continue;
        if (!isSpread && p.accessor == ast::AccessorKind::None) {
            propTypes.emplace_back(p.key, valT);
        }
        if (std::find(props.begin(), props.end(), p.key) == props.end()) {
            props.push_back(p.key);
        }
    }
    if (computedKey) return Type::dynamic();
    const ShapeClassId cls = mod_.result->shapes.intern(std::string(), std::move(props));
    if (scope_.moduleTopLevel) {
        for (const auto& p : o.props) {
            if (p.keyExpr || p.key.empty()) continue;
            if (o.isModuleNamespace && p.accessor == ast::AccessorKind::Getter) {
                if (const auto* f = ast::exactAs<const ast::FunctionExpr*>(p.value.get())) {
                    if (f->body.size() == 1) {
                        if (const auto* ret = ast::exactAs<const ast::ReturnStmt*>(f->body[0].get())) {
                            if (const auto* id = ast::exactAs<const ast::Ident*>(ret->value.get())) {
                                if (const auto it = mod_.indexByName.find(id->name);
                                    it != mod_.indexByName.end()) {
                                    auto& vec = mod_.objectLitMethods[{cls, p.key}];
                                    if (std::find(vec.begin(), vec.end(), it->second) == vec.end()) {
                                        vec.push_back(it->second);
                                        ++mod_.objectLitTableVersion;
                                        ++mod_.persistentWrites;
                                    }
                                } else if (mod_.lazyModuleVars.count(id->name) != 0) {
                                    auto& slot = mod_.objectLitModuleVars[{cls, p.key}];
                                    if (slot != id->name) {
                                        slot = id->name;
                                        ++mod_.objectLitTableVersion;
                                        ++mod_.persistentWrites;
                                    }
                                }
                            }
                        }
                    }
                }
                continue;
            }
            if (p.accessor != ast::AccessorKind::None) continue;
            if (const auto* id = ast::exactAs<const ast::Ident*>(p.value.get())) {
                if (const auto it = mod_.indexByName.find(id->name); it != mod_.indexByName.end()) {
                    auto& vec = mod_.objectLitMethods[{cls, p.key}];
                    if (std::find(vec.begin(), vec.end(), it->second) == vec.end()) {
                        vec.push_back(it->second);
                        ++mod_.objectLitTableVersion;
                        ++mod_.persistentWrites;
                    }
                }
            } else if (const auto* f = ast::exactAs<const ast::FunctionExpr*>(p.value.get())) {
                const uint32_t sIdx = mod_.methods.setterIndexOfNode(f);
                if (sIdx != kNoMethod) {
                    auto& vec = mod_.objectLitSetters[{cls, p.key}];
                    if (std::find(vec.begin(), vec.end(), sIdx) == vec.end()) {
                        vec.push_back(sIdx);
                        ++mod_.objectLitTableVersion;
                        ++mod_.persistentWrites;
                    }
                }
            }
        }
    }
    if (unclosed) {
        if (mod_.objectLitUnclosed.insert(cls).second) ++mod_.persistentWrites;
    } else if (mod_.objectLitUnclosed.count(cls) == 0) {
        auto& fields = mod_.objectLitFields[cls];
        for (const auto& [k, v] : propTypes) {
            Type& slot = fields[k];
            const Type joined = join(slot, v);
            if (joined != slot) {
                slot = joined;
                ++mod_.persistentWrites;
            }
        }
    }
    if (record_) mod_.result->siteShapes[&o] = cls;
    return Type::object(cls);
}

Type FlowAnalyzer::analyzeNested(const ast::Node& site, const std::string& declaredName,
                                 const std::vector<ast::Param>& params,
                                 const std::vector<ast::StmtPtr>& body, Span span, bool isGenerator,
                                 ShapeClassId thisClass, uint32_t methodIndex, uint32_t ctorIndex) {
    std::string name = declaredName;
    if (name.empty()) name = "<anon" + std::to_string(anonCounter_++) + ">";
    std::vector<const ast::Stmt*> borrowed;
    borrowed.reserve(body.size());
    for (const auto& s : body) borrowed.push_back(s.get());

    std::vector<Type> paramTypes(params.size(), Type::dynamic());
    uint32_t staticSetterIdx = kNoMethod;
    uint32_t nestedFnIdx = kNoFunctionIndex;
    if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(&site)) {
        if (const auto it = mod_.nestedFnByDecl.find(fd); it != mod_.nestedFnByDecl.end()) {
            nestedFnIdx = it->second;
            const NestedFnInfo& info = mod_.nestedFns[nestedFnIdx];
            if (InferMemo* memo = mod_.rec()) {
                memo->noteNestedFn(nestedFnIdx, info.version);
            }
            for (size_t i = 0; i < paramTypes.size() && i < info.signature.params.size(); ++i) {
                paramTypes[i] = info.signature.params[i];
            }
        }
    }
    if (ctorIndex != kNoCtor) {
        paramTypes = ctorParamTypes(ctorIndex, params.size());
    } else if (methodIndex != kNoMethod) {
        const MethodInfo& self = mod_.methods.methods()[methodIndex];
        if (InferMemo* memo = mod_.rec()) {
            memo->noteMethod(methodIndex, self.version, mod_.methodPoison.poisons(methodIndex));
        }
        if (self.plainParams && !mod_.methodPoison.poisons(methodIndex)) {
            for (size_t i = 0; i < paramTypes.size() && i < self.signature.params.size(); ++i) {
                const Type proven = self.signature.params[i];
                if (mod_.methodParamTypes) {
                    paramTypes[i] = proven;
                } else {
                    if (proven.is(TypeKind::Object) && proven.shapeClass() != kNoShapeClass) {
                        paramTypes[i] = Type::objectIdentityOnly(proven.shapeClass());
                    } else if (proven.is(TypeKind::Never)) {
                        paramTypes[i] = proven;
                    }
                }
            }
        }
    } else if (mod_.interprocIdent) {
        const uint32_t sIdx =
            mod_.methods.setterIndexOfNode(dynamic_cast<const ast::FunctionExpr*>(&site));
        if (sIdx != kNoMethod) {
            const SetterInfo& s = mod_.methods.setters()[sIdx];
            if (!s.isStatic) {
                if (InferMemo* memo = mod_.rec()) {
                    memo->noteSetter(sIdx, s.version);
                }
                if (s.plainParams && !paramTypes.empty() && mod_.methodParamTypes) {
                    paramTypes[0] = s.paramType;
                }
            } else if (s.className.empty()) {
                staticSetterIdx = sIdx;
                if (InferMemo* memo = mod_.rec()) {
                    memo->noteSetter(sIdx, s.version);
                }
                if (s.plainParams && !paramTypes.empty()) {
                    if (s.paramType.is(TypeKind::Object) && s.paramType.shapeClass() != kNoShapeClass) {
                        paramTypes[0] = Type::objectIdentityOnly(s.paramType.shapeClass());
                    } else if (s.paramType.is(TypeKind::Never) && !record_ && !mod_.auditSettled) {
                        paramTypes[0] = Type::never();
                    }
                }
            }
        }
    }
    FunctionAnalysisArgs args;
    args.parent = &scope_;
    args.qualifiedName = qualifiedName_ + "::" + name;
    args.moduleIndex = kNoFunctionIndex;
    args.site = &site;
    args.directCallable = false;
    args.params = &params;
    args.paramTypes = std::move(paramTypes);
    args.body = std::move(borrowed);
    args.span = span;
    args.record = record_;
    args.isGenerator = isGenerator;
    args.thisClass = thisClass;
    args.methodIndex = methodIndex;
    args.ctorIndex = ctorIndex;
    args.moduleTopLevel = false;
    const Type ret = analyzeFunction(mod_, args).returnType;
    if (nestedFnIdx != kNoFunctionIndex) {
        mod_.contribute(InferMemo::Contrib::NestedFnReturn, nestedFnIdx, 0, ret);
    }
    if (staticSetterIdx != kNoMethod) {
        Type setterRet = Type::dynamic();
        if (ret.is(TypeKind::Never)) {
            setterRet = Type::never();
        } else if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
            setterRet = Type::objectIdentityOnly(ret.shapeClass());
        }
        mod_.contribute(InferMemo::Contrib::SetterReturn, staticSetterIdx, 0, setterRet);
    }
    return ret;
}

void FlowAnalyzer::analyzeClassBody(const std::string& className,
                                    const std::vector<ast::ClassMethod>& methods) {
    ShapeClassId owner = kNoShapeClass;
    if (!className.empty()) {
        if (const ClassLayout* cl = mod_.result->classLayouts.byName(className)) {
            owner = cl->shapeClass;
        }
    }
    const bool checkClassReach =
        !record_ && !mod_.auditSettled && mod_.ctorParamTypes && mod_.valueFlow && !className.empty();
    if (checkClassReach) {
        const uint32_t classIdx = mod_.ctors.classIndexOf(className);
        if (classIdx != kNoCtor) {
            if (InferMemo* memo = mod_.rec()) {
                memo->noteClassReach(classIdx, mod_.ctors.classReach()[classIdx].version);
            }
        }
    }
    const bool classReached = !checkClassReach || mod_.ctors.isClassReached(className);
    for (const auto& m : methods) {
        if (m.keyExpr) expr(*m.keyExpr);
        if (!classReached && !m.isStatic && !m.isStaticBlock) {
            continue;
        }
        const ShapeClassId receiver = m.isStatic ? kNoShapeClass : owner;
        if (m.fn) {
            const uint32_t index =
                mod_.interprocIdent ? mod_.methods.indexOfNode(m.fn.get()) : kNoMethod;
            const uint32_t ctorIndex =
                mod_.ctorParamTypes ? mod_.ctors.indexOfNode(m.fn.get()) : kNoCtor;
            if (!record_ && !mod_.auditSettled && mod_.interprocIdent && mod_.valueFlow) {
                if (index != kNoMethod) {
                    const MethodInfo& self = mod_.methods.methods()[index];
                    if (InferMemo* memo = mod_.rec()) {
                        memo->noteMethod(index, self.version, mod_.methodPoison.poisons(index));
                    }
                    if (!self.reached) {
                        continue;
                    }
                } else {
                    const uint32_t sIdx = mod_.methods.setterIndexOfNode(m.fn.get());
                    if (sIdx != kNoMethod) {
                        const SetterInfo& s = mod_.methods.setters()[sIdx];
                        if (InferMemo* memo = mod_.rec()) {
                            memo->noteSetter(sIdx, s.version);
                        }
                        if (!s.reached) {
                            continue;
                        }
                    } else if (m.isStatic && !m.isStaticBlock && !m.computed() &&
                               !m.name.empty()) {
                        if (!mod_.methods.isStaticMethodReachable(className, m.name)) {
                            continue;
                        }
                    }
                }
            }
            const Type returned =
                analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span,
                              m.fn->isGenerator || m.fn->isAsync, receiver, index, ctorIndex);
            if (index != kNoMethod) {
                mod_.contribute(InferMemo::Contrib::MethodReturn, index, 0, returned);
            }
        } else if (m.init) {
            const ShapeClassId saved = scope_.thisClass;
            scope_.thisClass = receiver;
            expr(*m.init);
            scope_.thisClass = saved;
        }
    }
}

void FlowAnalyzer::notePropertyWrite(const std::string& propName, Type receiver, Type valType) {
    if (!mod_.interprocIdent || !mod_.valueFlow || propName.empty()) return;
    if (receiver.is(TypeKind::Never)) return;
    if (receiver.is(TypeKind::Array) || receiver.is(TypeKind::TypedArray) ||
        receiver.is(TypeKind::Number) || receiver.is(TypeKind::String) ||
        receiver.is(TypeKind::Bool) || receiver.is(TypeKind::Null) ||
        receiver.is(TypeKind::Undefined)) {
        return;
    }
    const ClassLayout* cl = nullptr;
    if (receiver.is(TypeKind::Object) && receiver.shapeClass() != kNoShapeClass) {
        cl = mod_.result->classLayouts.byShapeClass(receiver.shapeClass());
        if (cl == nullptr) return;
    }
    const auto& setters = mod_.methods.setters();
    for (uint32_t i = 0; i < setters.size(); ++i) {
        if (setters[i].isStatic || setters[i].propName != propName) continue;
        if (cl != nullptr && !mod_.methods.areClassesRelated(setters[i].className, cl->name)) {
            continue;
        }
        mod_.contribute(InferMemo::Contrib::SetterReached, i, 0, valType);
    }
}

void FlowAnalyzer::noteStaticMethodAccess(const ast::MemberAccess& m, Type base) {
    if (!mod_.interprocIdent || !mod_.valueFlow || m.isPrivate || m.property.empty()) return;
    if (base.is(TypeKind::Never)) return;
    if (base.is(TypeKind::Object)) {
        if (base.shapeClass() != kNoShapeClass) {
            if (InferMemo* memo = mod_.rec()) memo->noteObjectLitTable(mod_.objectLitTableVersion);
            if (const auto it = mod_.objectLitMethods.find({base.shapeClass(), m.property});
                it != mod_.objectLitMethods.end()) {
                for (const uint32_t fnIdx : it->second) {
                    mod_.contribute(InferMemo::Contrib::FnReached, fnIdx, 0, Type::never());
                }
            }
            if (const auto vIt = mod_.objectLitModuleVars.find({base.shapeClass(), m.property});
                vIt != mod_.objectLitModuleVars.end()) {
                if (mod_.reachedModuleVars.insert(vIt->second).second) {
                    ++mod_.persistentWrites;
                }
            }
            if (!suppressObjectLitSetterReach_) {
                if (const auto it = mod_.objectLitSetters.find({base.shapeClass(), m.property});
                    it != mod_.objectLitSetters.end()) {
                    for (const uint32_t sIdx : it->second) {
                        mod_.contribute(InferMemo::Contrib::SetterReached, sIdx, 0, Type::dynamic());
                    }
                }
            }
        }
        return;
    }
    if (base.is(TypeKind::Array) || base.is(TypeKind::TypedArray) ||
        base.is(TypeKind::Number) || base.is(TypeKind::String) || base.is(TypeKind::Bool) ||
        base.is(TypeKind::Null) || base.is(TypeKind::Undefined)) {
        return;
    }
    if (base.is(TypeKind::Dynamic) && !mod_.methods.isMethodName(m.property)) {
        if (InferMemo* memo = mod_.rec()) memo->noteObjectLitTable(mod_.objectLitTableVersion);
        for (const auto& [key, vec] : mod_.objectLitMethods) {
            if (key.second == m.property) {
                for (const uint32_t fnIdx : vec) {
                    mod_.contribute(InferMemo::Contrib::FnReached, fnIdx, 0, Type::never());
                }
            }
        }
        if (!suppressObjectLitSetterReach_) {
            for (const auto& [key, vec] : mod_.objectLitSetters) {
                if (key.second == m.property) {
                    for (const uint32_t sIdx : vec) {
                        mod_.contribute(InferMemo::Contrib::SetterReached, sIdx, 0, Type::dynamic());
                    }
                }
            }
        }
    }
    if (ast::exactAs<const ast::ThisExpr*>(m.object.get()) != nullptr &&
        scope_.thisClass != kNoShapeClass) {
        return;
    }
    const auto* id = ast::exactAs<const ast::Ident*>(m.object.get());
    const bool isKnownClass = id != nullptr && mod_.ctors.isClassName(id->name);
    const bool isThisStatic =
        ast::exactAs<const ast::ThisExpr*>(m.object.get()) != nullptr &&
        scope_.thisClass == kNoShapeClass;
    bool isCtorProp = false;
    if (const auto* inner = ast::exactAs<const ast::MemberAccess*>(m.object.get())) {
        if (inner->property == "constructor") isCtorProp = true;
    }
    const auto& setters = mod_.methods.setters();
    for (uint32_t i = 0; i < setters.size(); ++i) {
        const auto& s = setters[i];
        if (!s.isStatic || s.className.empty() || s.propName != m.property) continue;
        if (isKnownClass) {
            if (!mod_.methods.areClassesRelated(s.className, id->name)) continue;
        } else if (!isThisStatic && !isCtorProp && !base.is(TypeKind::Function)) {
            const uint32_t cIdx = mod_.ctors.classIndexOf(s.className);
            if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
                memo->noteCtor(cIdx, mod_.ctors.ctors()[cIdx].version,
                               mod_.ctorPoison.poisons(s.className));
            }
            if (!mod_.ctors.isClassReached(s.className) ||
                !mod_.ctorPoison.poisons(s.className)) {
                continue;
            }
        }
        mod_.contribute(InferMemo::Contrib::SetterReached, i, 0, Type::never());
    }
}

std::optional<Type> FlowAnalyzer::callObjectLitSetter(
    const ast::Call& c, const ast::MemberAccess* member, Type receiver,
    const std::vector<Type>& args, bool spreadArgs) {
    if (c.optional || !mod_.interprocIdent || !mod_.valueFlow) return std::nullopt;
    std::vector<uint32_t> targets;
    if (member != nullptr && !member->optional && !member->isPrivate && !member->property.empty()) {
        if (receiver.is(TypeKind::Object) && receiver.shapeClass() != kNoShapeClass &&
            mod_.result->classLayouts.byShapeClass(receiver.shapeClass()) == nullptr) {
            if (InferMemo* memo = mod_.rec()) memo->noteObjectLitTable(mod_.objectLitTableVersion);
            if (const auto it = mod_.objectLitSetters.find({receiver.shapeClass(), member->property});
                it != mod_.objectLitSetters.end()) {
                targets = it->second;
            }
        } else if (receiver.is(TypeKind::Dynamic) && !mod_.methods.isMethodName(member->property)) {
            if (InferMemo* memo = mod_.rec()) memo->noteObjectLitTable(mod_.objectLitTableVersion);
            for (const auto& [key, vec] : mod_.objectLitSetters) {
                if (key.second == member->property) {
                    targets.insert(targets.end(), vec.begin(), vec.end());
                }
            }
        }
    } else if (member == nullptr) {
        if (const auto* id = ast::exactAs<const ast::Ident*>(c.callee.get())) {
            if (!scope_.isCell(id->name)) {
                if (const auto it = scope_.objectLitMethodAliases.find(id->name);
                    it != scope_.objectLitMethodAliases.end()) {
                    if (InferMemo* memo = mod_.rec()) {
                        memo->noteObjectLitTable(mod_.objectLitTableVersion);
                    }
                    for (const auto& [key, vec] : mod_.objectLitSetters) {
                        if (key.second == it->second) {
                            targets.insert(targets.end(), vec.begin(), vec.end());
                        }
                    }
                }
            }
        }
    }
    if (targets.empty()) return std::nullopt;
    if (!spreadArgs && !args.empty() && args[0].is(TypeKind::Never)) {
        for (const uint32_t sIdx : targets) {
            if (InferMemo* memo = mod_.rec()) {
                memo->noteSetter(sIdx, mod_.methods.setters()[sIdx].version);
            }
        }
        return Type::never();
    }
    Type arg0 = Type::dynamic();
    if (!spreadArgs) {
        if (args.empty()) {
            arg0 = Type::undefined();
        } else if (args[0].is(TypeKind::Object) && args[0].shapeClass() != kNoShapeClass) {
            arg0 = Type::objectIdentityOnly(args[0].shapeClass());
        }
    }
    Type ret = Type::never();
    for (const uint32_t sIdx : targets) {
        mod_.contribute(InferMemo::Contrib::SetterReached, sIdx, 0, arg0);
        const SetterInfo& s = mod_.methods.setters()[sIdx];
        if (InferMemo* memo = mod_.rec()) memo->noteSetter(sIdx, s.version);
        ret = join(ret, s.returnType);
    }
    if (ret.is(TypeKind::Never)) return Type::never();
    if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
        return Type::objectIdentityOnly(ret.shapeClass());
    }
    return Type::dynamic();
}

std::vector<Type> FlowAnalyzer::ctorParamTypes(uint32_t ctorIndex, size_t count) const {
    std::vector<Type> out(count, Type::dynamic());
    if (!mod_.ctorParamTypes || ctorIndex == kNoCtor) return out;
    const CtorInfo& self = mod_.ctors.ctors()[ctorIndex];
    if (InferMemo* memo = mod_.rec()) {
        memo->noteCtor(ctorIndex, self.version, mod_.ctorPoison.poisons(self.className));
    }
    if (!self.plainParams || mod_.ctorPoison.poisons(self.className)) return out;
    for (size_t i = 0; i < count && i < self.signature.params.size(); ++i) {
        out[i] = self.signature.params[i];
    }
    return out;
}

void FlowAnalyzer::noteIdentRefusal(const ast::Ident& id, Type resolved) {
    if (!record_) return;
    if (!resolved.is(TypeKind::Dynamic)) return;

    if (mod_.interprocIdent && scope_.methodIndex != kNoMethod) {
        const MethodInfo& self = mod_.methods.methods()[scope_.methodIndex];
        bool isParam = false;
        for (const auto& p : self.fn->params) {
            if (p.name == id.name) {
                isParam = true;
                break;
            }
        }
        if (isParam) {
            for (size_t p = 0; p < self.safeParamNames.size(); ++p) {
                if (self.safeParamNames[p] != id.name) continue;
                if (p >= self.paramShapes.size()) break;
                const Type shape = self.paramShapes[p];
                if (shape.is(TypeKind::Object) && shape.shapeClass() != kNoShapeClass) {
                    mod_.result->guessedParamShapes.emplace(&id, shape.shapeClass());
                }
                break;
            }
            std::string reason;
            if (!self.plainParams) {
                reason = "the method's parameter list is not plain";
            } else if (mod_.methodPoison.poisons(scope_.methodIndex)) {
                reason = mod_.methodPoison.reasonFor(scope_.methodIndex);
            } else if (self.unreached) {
                reason = "no call site this compilation saw reaches the method";
            } else {
                reason = "the call sites disagree about the argument's class";
            }
            mod_.result->identRefusals.emplace(
                &id, "receiver is dynamic: parameter (" + reason + ")");
            return;
        }
    }

    for (const auto& p : facts_.paramNames) {
        if (p == id.name) {
            mod_.result->identRefusals.emplace(&id, "receiver is dynamic: function-value parameter");
            return;
        }
    }

    if (scope_.env.count(id.name) != 0 || scope_.cells.count(id.name) != 0) {
        mod_.result->identRefusals.emplace(&id, "receiver is dynamic: local binding");
        return;
    }
    for (const Scope* p = scope_.parent; p != nullptr; p = p->parent) {
        if (p->cells.count(id.name) != 0) {
            mod_.result->identRefusals.emplace(&id, "receiver is dynamic: captured binding");
            return;
        }
    }

    if (mod_.moduleScopeNames.count(id.name) != 0) {
        mod_.result->identRefusals.emplace(&id, "receiver is dynamic: module binding");
        return;
    }

    mod_.result->identRefusals.emplace(&id, "receiver is dynamic: global or host name");
}

const ClassLayout* FlowAnalyzer::receiverClass(Type receiver) const {
    if (!receiver.is(TypeKind::Object) || receiver.shapeClass() == kNoShapeClass) return nullptr;
    const ClassLayout* cl = mod_.result->classLayouts.byShapeClass(receiver.shapeClass());
    return cl == nullptr || cl->name.empty() ? nullptr : cl;
}

const PinKind* FlowAnalyzer::pinnedField(ShapeClassId cls, const std::string& field) const {
    if (mod_.pins == nullptr || cls == kNoShapeClass) return nullptr;
    const ClassLayout* layout = mod_.result->classLayouts.byShapeClass(cls);
    std::string name =
        layout != nullptr ? layout->name : mod_.result->shapes.at(cls).constructorName;
    if (name.empty()) return nullptr;
    for (uint32_t hop = 0; hop < kMaxExtendsHops; ++hop) {
        if (const PinKind* pin = mod_.pins->lookup(name, field)) return pin;
        if (layout == nullptr || layout->superName.empty()) return nullptr;
        layout = mod_.result->classLayouts.byName(layout->superName);
        if (layout == nullptr) return nullptr;
        name = layout->name;
    }
    return nullptr;
}

namespace {

class NestedCalleeScan final : public ast::detail::IdentVisitor {
public:
    explicit NestedCalleeScan(const ast::FunctionDecl* target)
        : target_(target), name_(target->name) {}

    std::vector<const ast::Call*> sites;
    bool escaped = false;
    bool spread = false;

    void mention(const std::string& name) override {
        if (name == name_) escaped = true;
    }

    void visit(const ast::Call& c) override {
        const auto* id = dynamic_cast<const ast::Ident*>(c.callee.get());
        if (id != nullptr && id->name == name_) {
            if (c.optional) escaped = true;
            for (const auto& arg : c.args) {
                if (dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) spread = true;
            }
            sites.push_back(&c);
        } else {
            c.callee->accept(*this);
        }
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const ast::FunctionDecl& f) override {
        if (&f != target_ && f.name == name_) escaped = true;
        for (const auto& p : f.params) {
            if (p.name == name_) escaped = true;
            if (p.pattern) {
                for (const auto& bn : ast::patternBoundNames(*p.pattern)) {
                    if (bn == name_) escaped = true;
                }
            }
        }
        ast::detail::visitParamExprs(f.params, *this);
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }

    void visit(const ast::FunctionExpr& f) override {
        if (f.name == name_) escaped = true;
        for (const auto& p : f.params) {
            if (p.name == name_) escaped = true;
            if (p.pattern) {
                for (const auto& bn : ast::patternBoundNames(*p.pattern)) {
                    if (bn == name_) escaped = true;
                }
            }
        }
        ast::detail::visitParamExprs(f.params, *this);
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }

    void visit(const ast::ClassExpr& c) override {
        if (c.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(c);
    }

private:
    const ast::FunctionDecl* target_ = nullptr;
    std::string name_;
};

void scanNestedFunctionScope(const std::vector<ast::Param>& params,
                             const std::vector<ast::StmtPtr>& stmts, ModuleContext& mod) {
    bool anyDecl = false;
    std::unordered_map<std::string, uint32_t> declCounts;
    for (const auto& stmt : stmts) {
        if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) {
            if (!fnDecl->name.empty()) {
                ++declCounts[fnDecl->name];
                if (!fnDecl->isGenerator && !fnDecl->isAsync) anyDecl = true;
            }
        }
    }
    if (!anyDecl) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyAssignedNames(stmts);
    for (const auto& p : params) {
        if (!p.name.empty()) rebound.insert(p.name);
        if (p.pattern) {
            for (const auto& bn : ast::patternBoundNames(*p.pattern)) rebound.insert(bn);
        }
        if (p.defaultValue) {
            for (auto& name : ast::getDeeplyAssignedNames(*p.defaultValue)) {
                rebound.insert(std::move(name));
            }
        }
    }

    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl == nullptr || fnDecl->isGenerator || fnDecl->isAsync || fnDecl->name.empty()) {
            continue;
        }
        if (declCounts[fnDecl->name] != 1 || rebound.count(fnDecl->name) != 0) continue;
        bool shapeFits = true;
        for (const auto& p : fnDecl->params) {
            if (p.defaultValue || p.isRest || p.pattern || p.name.empty() ||
                p.name == fnDecl->name) {
                shapeFits = false;
                break;
            }
        }
        if (!shapeFits) continue;

        NestedCalleeScan scan(fnDecl);
        for (const auto& s : stmts) {
            if (s) s->accept(scan);
        }
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(scan);
            if (p.pattern) ast::detail::visitPatternExprs(p.pattern.get(), scan);
        }
        if (scan.escaped || scan.spread || scan.sites.empty()) continue;

        const auto idx = static_cast<uint32_t>(mod.nestedFns.size());
        NestedFnInfo info;
        info.decl = fnDecl;
        info.signature.params.assign(fnDecl->params.size(), Type::never());
        info.signature.returnType = Type::never();
        info.observedParams.assign(fnDecl->params.size(), Type::never());
        mod.nestedFnByDecl.emplace(fnDecl, idx);
        for (const ast::Call* site : scan.sites) {
            mod.nestedFnByCall.emplace(site, idx);
        }
        mod.nestedFns.push_back(std::move(info));
    }
}

class NestedFnTableBuilder final : public Walker {
public:
    explicit NestedFnTableBuilder(ModuleContext& mod) : mod_(mod) {}

    void visit(const ast::FunctionDecl& n) override {
        scanNestedFunctionScope(n.params, n.body, mod_);
        Walker::visit(n);
    }

    void visit(const ast::FunctionExpr& n) override {
        scanNestedFunctionScope(n.params, n.body, mod_);
        Walker::visit(n);
    }

private:
    ModuleContext& mod_;
};

}  // namespace

void buildNestedFnTable(const ast::Module& module, ModuleContext& mod) {
    NestedFnTableBuilder builder(mod);
    builder.walkList(module.body);
}

}  // namespace bronze::types
