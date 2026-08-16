// The expression half of flow analysis: what type an expression produces,
// which callee a call site contributes an argument to, and which shape class
// a literal or a `new` interns. The statement walk is the other half and
// lives in flow.cpp; the seam is argued in flow_analyzer.h.

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "types/flow_analyzer.h"
#include "types/operator_types.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

// The ordered property names a constructor installs on `this`, in source order.
// Does not descend into nested functions: each one binds its own receiver, so
// an inner `this.x =` says nothing about this constructor.
//
// Conditional assignments are collected unconditionally, so this can name a
// class the runtime never builds. That is deliberate and safe: the inline-cache
// check keeps the shape guard even on a proven site, because the proof is over
// this compilation's source and the shape word is the runtime's authority.
class ThisPropertyWalker final : public Walker {
public:
    std::vector<std::string> properties;

    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::FunctionDecl&) override {}

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* member = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(member->object.get())) {
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

// ---- expressions -------------------------------------------------------

Type FlowAnalyzer::expr(const ast::Expr& e) {
    const Type t = exprKind(e);
    if (record_) mod_.result->exprTypes[&e] = t;
    return t;
}

Type FlowAnalyzer::exprKind(const ast::Expr& e) {
    if (dynamic_cast<const ast::NumberLit*>(&e)) return Type::number();
    // DYNAMIC, and deliberately not `number`: the lattice has no BigInt
    // element, and a BigInt typed as one would licence an f64 fast path to
    // read a heap pointer's bits as a double. Dynamic is the designed
    // fallback and the only answer here that cannot be wrong.
    if (dynamic_cast<const ast::BigIntLit*>(&e)) return Type::dynamic();
    if (dynamic_cast<const ast::StringLit*>(&e)) return Type::string();
    // A regular expression literal is an OBJECT, and inference has no shape
    // class for one: a RegExp carries no shape at all, so every read off it
    // goes through the runtime's own branch.
    if (dynamic_cast<const ast::RegExpLit*>(&e)) return Type::dynamic();
    // A template is a string whatever its substitutions produce, since
    // every one of them goes through ToString. The substitutions are
    // still analysed — they are ordinary expressions and may write
    // bindings.
    if (const auto* t = dynamic_cast<const ast::TemplateLit*>(&e)) {
        for (const auto& sub : t->exprs) expr(*sub);
        return Type::string();
    }
    if (dynamic_cast<const ast::BoolLit*>(&e)) return Type::boolean();
    if (dynamic_cast<const ast::NullLit*>(&e)) return Type::null();
    if (dynamic_cast<const ast::UndefinedLit*>(&e)) return Type::undefined();
    // `this` is the caller's receiver; nothing here proves anything about it.
    // Sharpening it needs the shape-class work applied to constructors, which
    // is the property-access step.
    if (dynamic_cast<const ast::ThisExpr*>(&e)) return Type::dynamic();
    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) return lookup(id->name);
    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) return unary(*u);
    if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) return binary(*b);
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&e)) {
        expr(*t->condition);
        const Env entry = scope_.env;
        const Type a = expr(*t->thenExpr);
        const Env thenEnv = scope_.env;
        scope_.env = entry;
        const Type b = expr(*t->elseExpr);
        scope_.env = joinEnv(thenEnv, scope_.env);
        return join(a, b);
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&e)) {
        expr(*m->object);
        // v1 proves the receiver's shape class, never the property's type; that
        // is what the inline-cache check consumes and all it needs.
        return Type::dynamic();
    }
    if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&e)) {
        expr(*ix->object);
        expr(*ix->index);
        return Type::dynamic();
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(&e)) return call(*c);
    if (const auto* n = dynamic_cast<const ast::NewExpr*>(&e)) return newExpr(*n);
    if (dynamic_cast<const ast::NewTargetExpr*>(&e)) return Type::dynamic();
    if (const auto* tt = dynamic_cast<const ast::TaggedTemplate*>(&e)) {
        expr(*tt->tag);
        for (const auto& el : tt->templateLit->exprs) expr(*el);
        return Type::dynamic();
    }
    if (const auto* o = dynamic_cast<const ast::ObjectLit*>(&e)) return objectLit(*o);
    if (const auto* a = dynamic_cast<const ast::ArrayLit*>(&e)) {
        for (const auto& el : a->elements) {
            if (el) expr(*el);
        }
        // An array is an object with no property-name identity; there is
        // no shape class to prove about it.
        return Type::object();
    }
    if (const auto* sc = dynamic_cast<const ast::SuperCall*>(&e)) {
        // The parent constructor runs on the current receiver and its
        // result is discarded, so nothing is proven about the value.
        for (const auto& a : sc->args) expr(*a);
        return Type::dynamic();
    }
    if (dynamic_cast<const ast::SuperMember*>(&e)) return Type::dynamic();
    if (const auto* ce = dynamic_cast<const ast::ClassExpr*>(&e)) {
        for (const auto& m : ce->methods) {
            if (m.keyExpr) expr(*m.keyExpr);
            if (m.fn) {
                analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span,
                              m.fn->isGenerator || m.fn->isAsync);
            } else if (m.init) {
                expr(*m.init);
            }
        }
        return Type::function();
    }
    // The value of a `yield` is the argument of the `next(v)` that resumed the
    // generator, which comes from outside this compilation entirely. Its
    // operand is still analysed: it is ordinary code that runs here.
    if (const auto* y = dynamic_cast<const ast::YieldExpr*>(&e)) {
        expr(*y->argument);
        return Type::dynamic();
    }
    if (const auto* di = dynamic_cast<const ast::DynamicImportExpr*>(&e)) {
        if (di->specifier) expr(*di->specifier);
        return Type::dynamic();
    }
    // A spread contributes its argument's effects and nothing about the
    // container's element types — there is no element type here to prove.
    if (const auto* sp = dynamic_cast<const ast::SpreadElement*>(&e)) {
        expr(*sp->argument);
        return Type::dynamic();
    }
    // Every name a destructuring assignment writes becomes dynamic: the pieces
    // come out of an indexed or keyed read, and this pass tracks no element or
    // property types to say anything narrower. Assigning rather than ignoring
    // is the point — a name proven numeric before must not stay numeric across
    // it.
    if (const auto* da = dynamic_cast<const ast::DestructuringAssign*>(&e)) {
        const Type value = expr(*da->value);
        for (const auto& name : ast::patternBoundNames(*da->pattern)) {
            assign(name, Type::dynamic());
        }
        return value;
    }
    if (const auto* f = dynamic_cast<const ast::FunctionExpr*>(&e)) {
        analyzeNested(*f, f->name, f->params, f->body, f->span,
                      f->isGenerator || f->isAsync);
        return Type::function();
    }
    fail(e.span, "saw an unknown expression node kind");
    return Type::dynamic();
}

Type FlowAnalyzer::unary(const ast::Unary& u) {
    const Type operand = expr(*u.operand);
    const Type result = unaryResult(u.op, operand);
    // The update forms are the one place a USE site sharpens a name: the
    // binding holds a number afterwards whatever it held before.
    if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec ||
        u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PostDec) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
            assign(id->name, Type::number());
        }
    }
    return result;
}

Type FlowAnalyzer::binary(const ast::Binary& b) {
    if (b.op == ast::BinaryOp::Assign) {
        const Type rhs = expr(*b.rhs);
        if (const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get())) {
            assign(id->name, rhs);
        } else {
            expr(*b.lhs);
        }
        return rhs;
    }
    if (ast::isCompoundAssignOp(b.op)) {
        const ast::BinaryOp plain = ast::compoundAssignBase(b.op);
        const Type rhs = expr(*b.rhs);
        const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get());
        const Type current = id != nullptr ? lookup(id->name) : expr(*b.lhs);
        const Type result = compoundResult(plain, current, rhs);
        if (id != nullptr) assign(id->name, result);
        return result;
    }

    // `&&` / `||` / `??` are short-circuiting, so the right operand may
    // not run; joining its environment back in would be a claim it did.
    // The value is one operand or the other, hence the type join.
    if (b.op == ast::BinaryOp::LogicalAnd || b.op == ast::BinaryOp::LogicalOr ||
        b.op == ast::BinaryOp::NullishCoalescing) {
        const Type l = expr(*b.lhs);
        const Env entry = scope_.env;
        const Type r = expr(*b.rhs);
        scope_.env = joinEnv(entry, scope_.env);
        return join(l, r);
    }

    const Type l = expr(*b.lhs);
    const Type r = expr(*b.rhs);
    return binaryResult(b.op, l, r);
}

Type FlowAnalyzer::call(const ast::Call& c) {
    const Type calleeType = expr(*c.callee);
    std::vector<Type> args;
    args.reserve(c.args.size());
    for (const auto& a : c.args) args.push_back(expr(*a));

    const uint32_t index = calleeType.functionIndex();
    if (index == kNoFunctionIndex) return Type::dynamic();

    FunctionInfo& callee = mod_.functions[index];
    if (!callee.directCallable) return Type::dynamic();

    // This site's contribution to the callee's parameters. A missing
    // argument is `undefined`, exactly as the call would deliver it.
    for (size_t i = 0; i < callee.observedParams.size(); ++i) {
        const Type at = i < args.size() ? args[i] : Type::undefined();
        callee.observedParams[i] = join(callee.observedParams[i], at);
    }
    return callee.signature.returnType;
}

Type FlowAnalyzer::newExpr(const ast::NewExpr& n) {
    // A bare NAME is the only callee whose constructor identity is knowable
    // here, and identity is what a shape class is: `new Foo()` names the
    // function whose `this.x = ...` assignments describe the layout, where
    // `new obj.Ctor()` names a value the analysis cannot follow back to one.
    // The unproven site gets `kNoShapeClass`, which is the same answer an
    // unknown name already produced — its property sites stay polymorphic
    // rather than guessing a layout.
    const auto* ident = dynamic_cast<const ast::Ident*>(n.callee.get());
    // The callee is evaluated before the arguments (ECMA-262 13.3.5.1), so
    // its effects are recorded first. A bare name is deliberately not walked:
    // reading it is not what `new` does with it, and `constructorShape` below
    // is the fact this site contributes about that name.
    if (ident == nullptr) expr(*n.callee);
    for (const auto& a : n.args) expr(*a);
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

ShapeClassId FlowAnalyzer::constructorShape(const std::string& name) {
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
        // Key then value, left to right, which is the order the language
        // specifies and therefore the order the effects are recorded in.
        if (p.keyExpr) {
            computedKey = true;
            expr(*p.keyExpr);
        }
        expr(*p.value);
        if (p.keyExpr) continue;
        // A duplicate key overwrites; it does not transition again.
        if (std::find(props.begin(), props.end(), p.key) == props.end()) {
            props.push_back(p.key);
        }
    }
    // A computed key names its property only at run time, so this literal's
    // own-property set is not known here. A shape class interned over the
    // WRITTEN keys alone would be a claim about a layout the runtime never
    // builds, and the inline caches rest on that claim being true — so a
    // literal with any computed key is simply `dynamic`, and its sites stay
    // polymorphic.
    if (computedKey) return Type::dynamic();
    // Empty constructor name: a plain literal's prototype is the one root shape
    // every `{}` shares.
    const ShapeClassId cls = mod_.result->shapes.intern(std::string(), std::move(props));
    if (record_) mod_.result->siteShapes[&o] = cls;
    return Type::object(cls);
}

void FlowAnalyzer::analyzeNested(const ast::Node& site, const std::string& declaredName,
                   const std::vector<ast::Param>& params,
                   const std::vector<ast::StmtPtr>& body, Span span, bool isGenerator) {
    std::string name = declaredName;
    if (name.empty()) name = "<anon" + std::to_string(anonCounter_++) + ">";
    std::vector<const ast::Stmt*> borrowed;
    borrowed.reserve(body.size());
    for (const auto& s : body) borrowed.push_back(s.get());

    // A closure is never a direct-call target, so its parameters keep the
    // uniform dynamic convention.
    const std::vector<Type> paramTypes(params.size(), Type::dynamic());
    analyzeFunction(mod_, &scope_, qualifiedName_ + "::" + name, kNoFunctionIndex, &site,
                    /*directCallable=*/false, params, paramTypes, borrowed, span, record_,
                    isGenerator);
}

}  // namespace bronze::types
