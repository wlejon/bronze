// Nested functions, object literals, constructor shapes and class body analysis
// for flow analysis. Decomposed from flow_expr.cpp to keep both files strictly
// under the line limit.

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
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
    bool computedKey = false;
    for (const auto& p : o.props) {
        if (p.keyExpr) {
            computedKey = true;
            expr(*p.keyExpr);
        }
        expr(*p.value);
        if (p.keyExpr) continue;
        if (std::find(props.begin(), props.end(), p.key) == props.end()) {
            props.push_back(p.key);
        }
    }
    if (computedKey) return Type::dynamic();
    const ShapeClassId cls = mod_.result->shapes.intern(std::string(), std::move(props));
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
    if (ctorIndex != kNoCtor) {
        paramTypes = ctorParamTypes(ctorIndex, params.size());
    } else if (methodIndex != kNoMethod) {
        const MethodInfo& self = mod_.methods.methods()[methodIndex];
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
    return analyzeFunction(mod_, args).returnType;
}

void FlowAnalyzer::analyzeClassBody(const std::string& className,
                                    const std::vector<ast::ClassMethod>& methods) {
    ShapeClassId owner = kNoShapeClass;
    if (!className.empty()) {
        if (const ClassLayout* cl = mod_.result->classLayouts.byName(className)) {
            owner = cl->shapeClass;
        }
    }
    for (const auto& m : methods) {
        if (m.keyExpr) expr(*m.keyExpr);
        const ShapeClassId receiver = m.isStatic ? kNoShapeClass : owner;
        if (m.fn) {
            const uint32_t index =
                mod_.interprocIdent ? mod_.methods.indexOfNode(m.fn.get()) : kNoMethod;
            const uint32_t ctorIndex =
                mod_.ctorParamTypes ? mod_.ctors.indexOfNode(m.fn.get()) : kNoCtor;
            const Type returned =
                analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span,
                              m.fn->isGenerator || m.fn->isAsync, receiver, index, ctorIndex);
            if (index != kNoMethod) {
                MethodInfo& self = mod_.methods.methods()[index];
                self.observedReturn = join(self.observedReturn, returned);
            }
        } else if (m.init) {
            const ShapeClassId saved = scope_.thisClass;
            scope_.thisClass = receiver;
            expr(*m.init);
            scope_.thisClass = saved;
        }
    }
}

}  // namespace bronze::types
