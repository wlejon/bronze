#include "ast/clone.h"

#include <utility>
#include <vector>

namespace bronze::ast {

PatternPtr clonePattern(const BindingPattern& pattern) {
    auto res = std::make_unique<BindingPattern>();
    res->isObject = pattern.isObject;
    res->span = pattern.span;
    for (const auto& elem : pattern.elements) {
        PatternElement pe;
        pe.name = elem.name;
        if (elem.pattern) pe.pattern = clonePattern(*elem.pattern);
        if (elem.target) pe.target = cloneExpr(*elem.target);
        pe.key = elem.key;
        if (elem.keyExpr) pe.keyExpr = cloneExpr(*elem.keyExpr);
        if (elem.defaultValue) pe.defaultValue = cloneExpr(*elem.defaultValue);
        pe.isRest = elem.isRest;
        pe.span = elem.span;
        res->elements.push_back(std::move(pe));
    }
    return res;
}

Param cloneParam(const Param& param) {
    Param res;
    res.name = param.name;
    res.typeAnnotation = param.typeAnnotation;
    if (param.defaultValue) res.defaultValue = cloneExpr(*param.defaultValue);
    if (param.pattern) res.pattern = clonePattern(*param.pattern);
    res.isRest = param.isRest;
    res.span = param.span;
    return res;
}

ClassMethod cloneClassMethod(const ClassMethod& method) {
    ClassMethod res;
    res.name = method.name;
    if (method.keyExpr) res.keyExpr = cloneExpr(*method.keyExpr);
    res.isStatic = method.isStatic;
    res.isConstructor = method.isConstructor;
    res.isField = method.isField;
    if (method.init) res.init = cloneExpr(*method.init);
    res.accessor = method.accessor;
    if (method.fn) {
        auto clonedFn = cloneExpr(*method.fn);
        res.fn.reset(static_cast<FunctionExpr*>(clonedFn.release()));
    }
    return res;
}

ExprPtr cloneExpr(const Expr& expr) {
    if (const auto* n = dynamic_cast<const NumberLit*>(&expr)) {
        auto res = std::make_unique<NumberLit>();
        res->span = n->span;
        res->parenthesized = n->parenthesized;
        res->value = n->value;
        return res;
    }
    if (const auto* s = dynamic_cast<const StringLit*>(&expr)) {
        auto res = std::make_unique<StringLit>();
        res->span = s->span;
        res->parenthesized = s->parenthesized;
        res->value = s->value;
        return res;
    }
    if (const auto* b = dynamic_cast<const BoolLit*>(&expr)) {
        auto res = std::make_unique<BoolLit>();
        res->span = b->span;
        res->parenthesized = b->parenthesized;
        res->value = b->value;
        return res;
    }
    if (const auto* nl = dynamic_cast<const NullLit*>(&expr)) {
        auto res = std::make_unique<NullLit>();
        res->span = nl->span;
        res->parenthesized = nl->parenthesized;
        return res;
    }
    if (const auto* ul = dynamic_cast<const UndefinedLit*>(&expr)) {
        auto res = std::make_unique<UndefinedLit>();
        res->span = ul->span;
        res->parenthesized = ul->parenthesized;
        return res;
    }
    if (const auto* t = dynamic_cast<const ThisExpr*>(&expr)) {
        auto res = std::make_unique<ThisExpr>();
        res->span = t->span;
        res->parenthesized = t->parenthesized;
        return res;
    }
    if (const auto* id = dynamic_cast<const Ident*>(&expr)) {
        auto res = std::make_unique<Ident>();
        res->span = id->span;
        res->parenthesized = id->parenthesized;
        res->name = id->name;
        return res;
    }
    if (const auto* re = dynamic_cast<const RegExpLit*>(&expr)) {
        auto res = std::make_unique<RegExpLit>();
        res->span = re->span;
        res->parenthesized = re->parenthesized;
        res->pattern = re->pattern;
        res->flags = re->flags;
        return res;
    }
    if (const auto* sp = dynamic_cast<const SpreadElement*>(&expr)) {
        auto res = std::make_unique<SpreadElement>();
        res->span = sp->span;
        res->parenthesized = sp->parenthesized;
        if (sp->argument) res->argument = cloneExpr(*sp->argument);
        return res;
    }
    if (const auto* tl = dynamic_cast<const TemplateLit*>(&expr)) {
        auto res = std::make_unique<TemplateLit>();
        res->span = tl->span;
        res->parenthesized = tl->parenthesized;
        res->quasis = tl->quasis;
        res->rawQuasis = tl->rawQuasis;
        for (const auto& e : tl->exprs) res->exprs.push_back(cloneExpr(*e));
        return res;
    }
    if (const auto* tt = dynamic_cast<const TaggedTemplate*>(&expr)) {
        auto res = std::make_unique<TaggedTemplate>();
        res->span = tt->span;
        res->parenthesized = tt->parenthesized;
        if (tt->tag) res->tag = cloneExpr(*tt->tag);
        if (tt->templateLit) {
            auto clonedTpl = cloneExpr(*tt->templateLit);
            res->templateLit.reset(static_cast<TemplateLit*>(clonedTpl.release()));
        }
        return res;
    }
    if (const auto* u = dynamic_cast<const Unary*>(&expr)) {
        auto res = std::make_unique<Unary>();
        res->span = u->span;
        res->parenthesized = u->parenthesized;
        res->op = u->op;
        if (u->operand) res->operand = cloneExpr(*u->operand);
        return res;
    }
    if (const auto* bin = dynamic_cast<const Binary*>(&expr)) {
        auto res = std::make_unique<Binary>();
        res->span = bin->span;
        res->parenthesized = bin->parenthesized;
        res->op = bin->op;
        if (bin->lhs) res->lhs = cloneExpr(*bin->lhs);
        if (bin->rhs) res->rhs = cloneExpr(*bin->rhs);
        return res;
    }
    if (const auto* ter = dynamic_cast<const Ternary*>(&expr)) {
        auto res = std::make_unique<Ternary>();
        res->span = ter->span;
        res->parenthesized = ter->parenthesized;
        if (ter->condition) res->condition = cloneExpr(*ter->condition);
        if (ter->thenExpr) res->thenExpr = cloneExpr(*ter->thenExpr);
        if (ter->elseExpr) res->elseExpr = cloneExpr(*ter->elseExpr);
        return res;
    }
    if (const auto* mem = dynamic_cast<const MemberAccess*>(&expr)) {
        auto res = std::make_unique<MemberAccess>();
        res->span = mem->span;
        res->parenthesized = mem->parenthesized;
        if (mem->object) res->object = cloneExpr(*mem->object);
        res->property = mem->property;
        res->optional = mem->optional;
        return res;
    }
    if (const auto* idx = dynamic_cast<const IndexAccess*>(&expr)) {
        auto res = std::make_unique<IndexAccess>();
        res->span = idx->span;
        res->parenthesized = idx->parenthesized;
        if (idx->object) res->object = cloneExpr(*idx->object);
        if (idx->index) res->index = cloneExpr(*idx->index);
        res->optional = idx->optional;
        return res;
    }
    if (const auto* c = dynamic_cast<const Call*>(&expr)) {
        auto res = std::make_unique<Call>();
        res->span = c->span;
        res->parenthesized = c->parenthesized;
        if (c->callee) res->callee = cloneExpr(*c->callee);
        for (const auto& a : c->args) res->args.push_back(cloneExpr(*a));
        res->optional = c->optional;
        return res;
    }
    if (const auto* ne = dynamic_cast<const NewExpr*>(&expr)) {
        auto res = std::make_unique<NewExpr>();
        res->span = ne->span;
        res->parenthesized = ne->parenthesized;
        if (ne->callee) res->callee = cloneExpr(*ne->callee);
        for (const auto& a : ne->args) res->args.push_back(cloneExpr(*a));
        return res;
    }
    if (const auto* nt = dynamic_cast<const NewTargetExpr*>(&expr)) {
        auto res = std::make_unique<NewTargetExpr>();
        res->span = nt->span;
        res->parenthesized = nt->parenthesized;
        return res;
    }
    if (const auto* sc = dynamic_cast<const SuperCall*>(&expr)) {
        auto res = std::make_unique<SuperCall>();
        res->span = sc->span;
        res->parenthesized = sc->parenthesized;
        res->baseName = sc->baseName;
        for (const auto& a : sc->args) res->args.push_back(cloneExpr(*a));
        return res;
    }
    if (const auto* sm = dynamic_cast<const SuperMember*>(&expr)) {
        auto res = std::make_unique<SuperMember>();
        res->span = sm->span;
        res->parenthesized = sm->parenthesized;
        res->baseName = sm->baseName;
        res->property = sm->property;
        return res;
    }
    if (const auto* y = dynamic_cast<const YieldExpr*>(&expr)) {
        auto res = std::make_unique<YieldExpr>();
        res->span = y->span;
        res->parenthesized = y->parenthesized;
        if (y->argument) res->argument = cloneExpr(*y->argument);
        res->delegate = y->delegate;
        res->isAwait = y->isAwait;
        return res;
    }
    if (const auto* d = dynamic_cast<const DestructuringAssign*>(&expr)) {
        auto res = std::make_unique<DestructuringAssign>();
        res->span = d->span;
        res->parenthesized = d->parenthesized;
        if (d->pattern) res->pattern = clonePattern(*d->pattern);
        if (d->value) res->value = cloneExpr(*d->value);
        return res;
    }
    if (const auto* di = dynamic_cast<const DynamicImportExpr*>(&expr)) {
        auto res = std::make_unique<DynamicImportExpr>();
        res->span = di->span;
        res->parenthesized = di->parenthesized;
        if (di->specifier) res->specifier = cloneExpr(*di->specifier);
        return res;
    }
    if (const auto* obj = dynamic_cast<const ObjectLit*>(&expr)) {
        auto res = std::make_unique<ObjectLit>();
        res->span = obj->span;
        res->parenthesized = obj->parenthesized;
        res->isModuleNamespace = obj->isModuleNamespace;
        for (const auto& p : obj->props) {
            ObjectProp prop;
            prop.key = p.key;
            if (p.keyExpr) prop.keyExpr = cloneExpr(*p.keyExpr);
            if (p.value) prop.value = cloneExpr(*p.value);
            prop.coverInitialized = p.coverInitialized;
            prop.isMethod = p.isMethod;
            prop.accessor = p.accessor;
            res->props.push_back(std::move(prop));
        }
        return res;
    }
    if (const auto* arr = dynamic_cast<const ArrayLit*>(&expr)) {
        auto res = std::make_unique<ArrayLit>();
        res->span = arr->span;
        res->parenthesized = arr->parenthesized;
        for (const auto& elem : arr->elements) {
            res->elements.push_back(elem ? cloneExpr(*elem) : nullptr);
        }
        return res;
    }
    if (const auto* fn = dynamic_cast<const FunctionExpr*>(&expr)) {
        auto res = std::make_unique<FunctionExpr>();
        res->span = fn->span;
        res->parenthesized = fn->parenthesized;
        res->name = fn->name;
        for (const auto& p : fn->params) res->params.push_back(cloneParam(p));
        res->returnType = fn->returnType;
        for (const auto& s : fn->body) res->body.push_back(cloneStmt(*s));
        res->isArrow = fn->isArrow;
        res->strict = fn->strict;
        res->isGenerator = fn->isGenerator;
        res->isAsync = fn->isAsync;
        return res;
    }
    if (const auto* ce = dynamic_cast<const ClassExpr*>(&expr)) {
        auto res = std::make_unique<ClassExpr>();
        res->span = ce->span;
        res->parenthesized = ce->parenthesized;
        res->name = ce->name;
        res->superName = ce->superName;
        for (const auto& m : ce->methods) res->methods.push_back(cloneClassMethod(m));
        return res;
    }
    return nullptr;
}

StmtPtr cloneStmt(const Stmt& stmt) {
    if (const auto* b = dynamic_cast<const BlockStmt*>(&stmt)) {
        auto res = std::make_unique<BlockStmt>();
        res->span = b->span;
        for (const auto& s : b->stmts) res->stmts.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* v = dynamic_cast<const VarDecl*>(&stmt)) {
        auto res = std::make_unique<VarDecl>();
        res->span = v->span;
        res->isConst = v->isConst;
        res->isVar = v->isVar;
        res->name = v->name;
        if (v->pattern) res->pattern = clonePattern(*v->pattern);
        res->typeAnnotation = v->typeAnnotation;
        if (v->init) res->init = cloneExpr(*v->init);
        return res;
    }
    if (const auto* r = dynamic_cast<const ReturnStmt*>(&stmt)) {
        auto res = std::make_unique<ReturnStmt>();
        res->span = r->span;
        if (r->value) res->value = cloneExpr(*r->value);
        return res;
    }
    if (const auto* e = dynamic_cast<const ExprStmt*>(&stmt)) {
        auto res = std::make_unique<ExprStmt>();
        res->span = e->span;
        if (e->expr) res->expr = cloneExpr(*e->expr);
        return res;
    }
    if (const auto* ifs = dynamic_cast<const IfStmt*>(&stmt)) {
        auto res = std::make_unique<IfStmt>();
        res->span = ifs->span;
        if (ifs->condition) res->condition = cloneExpr(*ifs->condition);
        for (const auto& s : ifs->thenBody) res->thenBody.push_back(cloneStmt(*s));
        for (const auto& s : ifs->elseBody) res->elseBody.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(&stmt)) {
        auto res = std::make_unique<WhileStmt>();
        res->span = w->span;
        if (w->condition) res->condition = cloneExpr(*w->condition);
        for (const auto& s : w->body) res->body.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* dw = dynamic_cast<const DoWhileStmt*>(&stmt)) {
        auto res = std::make_unique<DoWhileStmt>();
        res->span = dw->span;
        for (const auto& s : dw->body) res->body.push_back(cloneStmt(*s));
        if (dw->condition) res->condition = cloneExpr(*dw->condition);
        return res;
    }
    if (const auto* f = dynamic_cast<const ForStmt*>(&stmt)) {
        auto res = std::make_unique<ForStmt>();
        res->span = f->span;
        for (const auto& s : f->init) res->init.push_back(cloneStmt(*s));
        if (f->condition) res->condition = cloneExpr(*f->condition);
        if (f->update) res->update = cloneExpr(*f->update);
        for (const auto& s : f->body) res->body.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* brk = dynamic_cast<const BreakStmt*>(&stmt)) {
        auto res = std::make_unique<BreakStmt>();
        res->span = brk->span;
        res->label = brk->label;
        return res;
    }
    if (const auto* cont = dynamic_cast<const ContinueStmt*>(&stmt)) {
        auto res = std::make_unique<ContinueStmt>();
        res->span = cont->span;
        res->label = cont->label;
        return res;
    }
    if (const auto* sw = dynamic_cast<const SwitchStmt*>(&stmt)) {
        auto res = std::make_unique<SwitchStmt>();
        res->span = sw->span;
        if (sw->discriminant) res->discriminant = cloneExpr(*sw->discriminant);
        for (const auto& c : sw->cases) {
            SwitchCase sc;
            if (c.test) sc.test = cloneExpr(*c.test);
            for (const auto& s : c.body) sc.body.push_back(cloneStmt(*s));
            sc.span = c.span;
            res->cases.push_back(std::move(sc));
        }
        return res;
    }
    if (const auto* fi = dynamic_cast<const ForInStmt*>(&stmt)) {
        auto res = std::make_unique<ForInStmt>();
        res->span = fi->span;
        res->name = fi->name;
        if (fi->pattern) res->pattern = clonePattern(*fi->pattern);
        res->isConst = fi->isConst;
        res->isLet = fi->isLet;
        res->isVar = fi->isVar;
        if (fi->object) res->object = cloneExpr(*fi->object);
        for (const auto& s : fi->body) res->body.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* fo = dynamic_cast<const ForOfStmt*>(&stmt)) {
        auto res = std::make_unique<ForOfStmt>();
        res->span = fo->span;
        res->name = fo->name;
        if (fo->pattern) res->pattern = clonePattern(*fo->pattern);
        res->isConst = fo->isConst;
        res->isLet = fo->isLet;
        res->isVar = fo->isVar;
        res->isAwait = fo->isAwait;
        if (fo->iterable) res->iterable = cloneExpr(*fo->iterable);
        for (const auto& s : fo->body) res->body.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* lab = dynamic_cast<const LabeledStmt*>(&stmt)) {
        auto res = std::make_unique<LabeledStmt>();
        res->span = lab->span;
        res->label = lab->label;
        if (lab->body) res->body = cloneStmt(*lab->body);
        return res;
    }
    if (const auto* t = dynamic_cast<const TryStmt*>(&stmt)) {
        auto res = std::make_unique<TryStmt>();
        res->span = t->span;
        for (const auto& s : t->body) res->body.push_back(cloneStmt(*s));
        res->hasCatch = t->hasCatch;
        res->hasCatchParam = t->hasCatchParam;
        res->catchName = t->catchName;
        if (t->catchPattern) res->catchPattern = clonePattern(*t->catchPattern);
        for (const auto& s : t->catchBody) res->catchBody.push_back(cloneStmt(*s));
        res->hasFinally = t->hasFinally;
        for (const auto& s : t->finallyBody) res->finallyBody.push_back(cloneStmt(*s));
        return res;
    }
    if (const auto* th = dynamic_cast<const ThrowStmt*>(&stmt)) {
        auto res = std::make_unique<ThrowStmt>();
        res->span = th->span;
        if (th->value) res->value = cloneExpr(*th->value);
        return res;
    }
    if (const auto* cd = dynamic_cast<const ClassDecl*>(&stmt)) {
        auto res = std::make_unique<ClassDecl>();
        res->span = cd->span;
        res->name = cd->name;
        res->superName = cd->superName;
        for (const auto& m : cd->methods) res->methods.push_back(cloneClassMethod(m));
        return res;
    }
    if (const auto* fd = dynamic_cast<const FunctionDecl*>(&stmt)) {
        auto res = std::make_unique<FunctionDecl>();
        res->span = fd->span;
        res->isExported = fd->isExported;
        res->name = fd->name;
        for (const auto& p : fd->params) res->params.push_back(cloneParam(p));
        res->returnType = fd->returnType;
        for (const auto& s : fd->body) res->body.push_back(cloneStmt(*s));
        res->strict = fd->strict;
        res->isGenerator = fd->isGenerator;
        res->isAsync = fd->isAsync;
        return res;
    }
    return nullptr;
}

}  // namespace bronze::ast
