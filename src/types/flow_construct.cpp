// The constructor and `new` half of flow analysis: `new` expressions, constructor
// call sites, and argument propagation into constructor parameters.
// The expression walk is in flow_expr.cpp; the seam is argued in flow_analyzer.h.

#include "ast/queries.h"
#include "types/flow_analyzer.h"
#include "types/walk.h"

namespace bronze::types {

// One `new <a class name>(...)`, and every `super(...)` that reaches one.
//
// The site's arguments join into the parameters of whatever constructor the
// name positionally reaches — the class's own, or, when it declares none, the
// one its base reaches, because the implicit constructor forwards everything.
void FlowAnalyzer::constructSite(const std::string& className, const std::vector<Type>& args,
                                 bool spreadArgs) {
    if (!mod_.ctorParamTypes) return;
    const uint32_t target = mod_.ctors.targetOf(className);
    if (target != kNoCtor) contributeCtorArgs(target, args, spreadArgs);
}

// A `new` whose callee is a VALUE: `new this.constructor()`, `new Curves[t]()`,
// `new Ctor(x)` through a parameter.
//
// Which constructors such a site can reach is the whole question this chunk had
// to get right, and the answer is not "all of them". A constructor value gets
// into circulation two ways. The first is a read of the class binding, and that
// read is an escape the scan already poisoned the class for — so those classes
// have nothing left to give up. The second is a read off an OBJECT
// (`o.constructor`, `Object.getPrototypeOf(o)`, `new.target`, a computed read
// that could name `constructor`), and a program that does none of those cannot
// reach an unpoisoned class here at all.
//
// `new <recv>.constructor(...)` is the one form worth being precise about,
// because it is how three.js's `clone()` is written on half its classes: the
// classes it reaches are the ones `recv` can be, which is its class and
// everything that extends it.
void FlowAnalyzer::constructUnbounded(const ast::Expr& callee, Type calleeBase,
                                      const std::vector<Type>& args, bool spreadArgs) {
    if (!mod_.ctorParamTypes) return;
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&callee)) {
        if (m->property == "constructor" && !m->optional) {
            if (const ClassLayout* cls = receiverClass(calleeBase)) {
                std::vector<uint32_t> targets;
                mod_.ctors.subtreeOf(cls->name, targets);
                for (const uint32_t target : targets) contributeCtorArgs(target, args, spreadArgs);
                if (record_) ++mod_.unnamedNewSubtree;
                return;
            }
        }
    }
    if (!mod_.ctorEscapes.valueEscapes) {
        if (record_) ++mod_.unnamedNewIgnored;
        return;
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&callee)) {
        // A top-level `function` used as a constructor — which is how three.js
        // writes its whole WebGL back end, `new WebGLTextures(...)` and thirty
        // more. The name resolves to a module function and to nothing else, so
        // the object it builds is that function's `this`, and no CLASS
        // constructor is reached at all.
        if (lookup(id->name).functionIndex() != kNoFunctionIndex) {
            if (record_) ++mod_.unnamedNewIgnored;
            return;
        }
        // A name the program does not bind is a GLOBAL: `new Error(msg)`,
        // `new Map()`, `new Float64Array(n)`. It can hold one of the program's
        // own classes only if the program writes globals, which is a fact about
        // the text and not about this site.
        if (!resolvesToUserBinding(id->name) && !mod_.ctorEscapes.freeGlobalWrite) {
            if (record_) ++mod_.unnamedNewIgnored;
            return;
        }
    }
    if (spreadArgs) {
        mod_.ctorPoison.addAll("an argument list is spread at a `new` whose callee is a value");
        return;
    }
    if (record_) ++mod_.unnamedNewAll;
    for (uint32_t i = 0; i < mod_.ctors.ctors().size(); ++i) {
        contributeCtorArgs(i, args, /*spreadArgs=*/false);
    }
}

void FlowAnalyzer::contributeCtorArgs(uint32_t ctorIndex, const std::vector<Type>& args,
                                      bool spreadArgs) {
    CtorInfo& info = mod_.ctors.ctors()[ctorIndex];
    if (spreadArgs) {
        mod_.ctorPoison.add(info.className, "an argument list is spread at a construction site");
        return;
    }
    for (size_t i = 0; i < info.observedParams.size(); ++i) {
        if (i < args.size()) {
            // `undefined` at a defaulted position runs the DEFAULT, so the
            // value bound here is not the argument and the argument is not
            // evidence. The default's own type is joined in by the walk that
            // evaluates it (`runParamDefaults`).
            if (info.hasDefault[i] && args[i].is(TypeKind::Undefined)) continue;
            info.observedParams[i] = join(info.observedParams[i], args[i]);
            continue;
        }
        // A missing argument at a position with no default binds `undefined`,
        // exactly as the construction delivers it.
        if (!info.hasDefault[i]) {
            info.observedParams[i] = join(info.observedParams[i], Type::undefined());
        }
    }
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
    //
    // The RECEIVER of a member callee is captured here rather than after the
    // arguments, because walking them overwrites `lastMember_`. It is what
    // bounds `new x.constructor()` to the classes `x` can be.
    Type calleeBase = Type::dynamic();
    if (ident == nullptr) {
        expr(*n.callee);
        const auto* member = dynamic_cast<const ast::MemberAccess*>(n.callee.get());
        if (member != nullptr && lastMember_ == member) calleeBase = lastMemberBase_;
    }
    std::vector<Type> args;
    args.reserve(n.args.size());
    bool spreadArgs = false;
    for (const auto& a : n.args) {
        if (dynamic_cast<const ast::SpreadElement*>(a.get())) spreadArgs = true;
        args.push_back(expr(*a));
    }
    // This site's contribution to the constructor's parameters (ctor_ident.h).
    if (ident != nullptr && mod_.ctors.isClassName(ident->name)) {
        constructSite(ident->name, args, spreadArgs);
    } else {
        constructUnbounded(*n.callee, calleeBase, args, spreadArgs);
    }
    // `new Float64Array(...)` on the UNSHADOWED name is the builtin: assigning
    // to a builtin global is a compile error, so the only way the name can
    // mean anything else is a program binding — which `resolvesToUserBinding`
    // sees, module scope and imports included. The argument forms all produce
    // a view with this element kind, so the arguments do not matter here.
    if (ident != nullptr && !resolvesToUserBinding(ident->name)) {
        if (ident->name == "Float64Array") return Type::typedArray(TypedArrayElem::Float64);
        if (ident->name == "Float32Array") return Type::typedArray(TypedArrayElem::Float32);
        if (ident->name == "Int32Array") return Type::typedArray(TypedArrayElem::Int32);
        if (ident->name == "Uint32Array") return Type::typedArray(TypedArrayElem::Uint32);
        if (ident->name == "Int16Array") return Type::typedArray(TypedArrayElem::Int16);
        if (ident->name == "Uint16Array") return Type::typedArray(TypedArrayElem::Uint16);
        if (ident->name == "Int8Array") return Type::typedArray(TypedArrayElem::Int8);
        if (ident->name == "Uint8Array") return Type::typedArray(TypedArrayElem::Uint8);
        if (ident->name == "Uint8ClampedArray") return Type::typedArray(TypedArrayElem::Uint8Clamped);
        if (ident->name == "Array") return Type::array();
    }
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

}  // namespace bronze::types
