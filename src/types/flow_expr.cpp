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
                // `this.#x = v` installs no PROPERTY: a private element lives
                // in a side table keyed by the class evaluation, and no shape
                // ever carries its name. A shape class that listed it would
                // describe a layout no instance has.
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
    // `this` is the caller's receiver. Inside a class body the DECLARATION
    // names the object kind that receiver is meant to be, and `Scope::thisClass`
    // carries it — see the standing invariant recorded there for why an
    // optimistic answer is safe, and what would stop making it safe.
    if (dynamic_cast<const ast::ThisExpr*>(&e)) {
        return scope_.thisClass == kNoShapeClass ? Type::dynamic()
                                                 : Type::object(scope_.thisClass);
    }
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
        const Type base = expr(*m->object);
        // The Math VALUE properties are numbers on the pristine builtin —
        // 21.3.1 lists exactly these eight, all non-writable and
        // non-configurable, though what carries the proof here is the
        // program-wide pristine bit, not the attributes.
        if (!m->optional && isPristineMathBase(*m->object)) {
            if (m->property == "PI" || m->property == "E" || m->property == "LN2" ||
                m->property == "LN10" || m->property == "LOG2E" || m->property == "LOG10E" ||
                m->property == "SQRT1_2" || m->property == "SQRT2") {
                return Type::number();
            }
        }
        // A field of a class the layout analysis modelled: the type joined over
        // every `this.<name> = ...` the class body writes. This is what carries
        // a class identity ACROSS a property read — `object3d.position` is a
        // `Vector3`, so `object3d.position.x` is a proven site too, and the
        // chain of reads three.js is made of stops being one dynamic hop per
        // link. Anything unmodelled joined to `Dynamic` on the way in, so the
        // miss answer is the same one this returned before.
        //
        // Deliberately not applied to `?.`: an optional read of an absent link
        // produces `undefined`, which is not the field's type.
        if (!m->optional && base.is(TypeKind::Object) &&
            base.shapeClass() != kNoShapeClass && !m->isPrivate) {
            return mod_.result->classLayouts.fieldTypeOf(base.shapeClass(), m->property);
        }
        // Otherwise: the receiver's shape class is proven, never the property's
        // type; that is what the inline-cache check consumes and all it needs.
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
    if (dynamic_cast<const ast::ImportMetaExpr*>(&e)) return Type::dynamic();
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
        analyzeClassBody(ce->name, ce->methods);
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
        // 15.3.4: an arrow has no `this` binding of its own and resolves the
        // name in the enclosing scope, so it inherits whatever receiver is in
        // hand. A non-arrow function expression binds its own, which the
        // caller supplies and no declaration here can name.
        analyzeNested(*f, f->name, f->params, f->body, f->span,
                      f->isGenerator || f->isAsync,
                      f->isArrow ? scope_.thisClass : kNoShapeClass);
        return Type::function();
    }
    fail(e.span, "saw an unknown expression node kind");
    return Type::dynamic();
}

Type FlowAnalyzer::unary(const ast::Unary& u) {
    const Type operand = expr(*u.operand);
    const Type result = unaryResult(u.op, operand);
    // The update forms are the one place a USE site sharpens a name — but
    // only to what 13.4.4 actually stores: ToNumeric of the old value, which
    // `unaryResult` already computed, and which is a Number only when the
    // operand can never be a BigInt. Writing `number` unconditionally here
    // was the unsound claim that let `let c = 1n; c++` unbox a BigInt as an
    // f64 wherever a later read consumed the proof.
    if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec ||
        u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PostDec) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
            assign(id->name, result);
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

    // `Math.<fn>(...)` on the pristine builtin: every OWN function property
    // of 21.3 returns a Number for ANY arguments (a BigInt argument throws
    // before a value exists, which claims nothing). The list is the own
    // methods only — an INHERITED call like `Math.toString()` reaches
    // Object.prototype and is deliberately absent.
    if (!c.optional && mathCallReturnsNumber(c)) {
        if (record_) mod_.result->pristineMathCalls.insert(&c);
        return Type::number();
    }

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
    // `new Float64Array(...)` on the UNSHADOWED name is the builtin: assigning
    // to a builtin global is a compile error, so the only way the name can
    // mean anything else is a program binding — which `resolvesToUserBinding`
    // sees, module scope and imports included. The argument forms all produce
    // a view with this element kind, so the arguments do not matter here.
    if (ident != nullptr && !resolvesToUserBinding(ident->name)) {
        if (ident->name == "Float64Array") return Type::typedArray(TypedArrayElem::Float64);
        if (ident->name == "Float32Array") return Type::typedArray(TypedArrayElem::Float32);
    }
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

ShapeClassId FlowAnalyzer::constructorShape(const std::string& name) {
    // A `class` first: it is the form three.js is written in, and its identity
    // is already interned by the layout analysis, which knows things this
    // walker does not — the `extends` prefix, and field declarations.
    if (const ClassLayout* cl = mod_.result->classLayouts.byName(name)) {
        // A class name that is also a module function name is impossible: both
        // are module-scope bindings and the parser would have rejected the
        // redeclaration. Checking the class table first is therefore an
        // ordering, not a precedence rule.
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
                   const std::vector<ast::StmtPtr>& body, Span span, bool isGenerator,
                   ShapeClassId thisClass) {
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
                    isGenerator, thisClass);
}

// A class body, walked with `this` bound to the class the declaration names.
//
// One routine for `ClassDecl` and `ClassExpr` because the two nodes differ in
// nothing this pass reads. The receiver goes to instance members only: a static
// method's `this` is the CONSTRUCTOR, whose own layout is a different object's,
// and this analysis models instances.
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
            analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span,
                          m.fn->isGenerator || m.fn->isAsync, receiver);
        } else if (m.init) {
            // A field initializer is evaluated with `this` already bound to the
            // instance being constructed (15.7.10), so it sees the receiver too
            // — but it runs in THIS walker's scope, not a nested one, so the
            // binding has to be installed and taken back around it.
            const ShapeClassId saved = scope_.thisClass;
            scope_.thisClass = receiver;
            expr(*m.init);
            scope_.thisClass = saved;
        }
    }
}

}  // namespace bronze::types
