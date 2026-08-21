#include "ast/clone.h"

#include <utility>
#include <vector>

namespace bronze::ast {

PatternPtr clonePattern(const BindingPattern& pattern, CloneOrigins* origins) {
    auto res = std::make_unique<BindingPattern>();
    res->isObject = pattern.isObject;
    res->span = pattern.span;
    for (const auto& elem : pattern.elements) {
        PatternElement pe;
        pe.name = elem.name;
        if (elem.pattern) pe.pattern = clonePattern(*elem.pattern, origins);
        if (elem.target) pe.target = cloneExpr(*elem.target, origins);
        pe.key = elem.key;
        if (elem.keyExpr) pe.keyExpr = cloneExpr(*elem.keyExpr, origins);
        if (elem.defaultValue) pe.defaultValue = cloneExpr(*elem.defaultValue, origins);
        pe.isRest = elem.isRest;
        pe.span = elem.span;
        res->elements.push_back(std::move(pe));
    }
    return res;
}

Param cloneParam(const Param& param, CloneOrigins* origins) {
    Param res;
    res.name = param.name;
    res.typeAnnotation = param.typeAnnotation;
    if (param.defaultValue) res.defaultValue = cloneExpr(*param.defaultValue, origins);
    if (param.pattern) res.pattern = clonePattern(*param.pattern, origins);
    res.isRest = param.isRest;
    res.span = param.span;
    return res;
}

ClassMethod cloneClassMethod(const ClassMethod& method, CloneOrigins* origins) {
    ClassMethod res;
    res.name = method.name;
    if (method.keyExpr) res.keyExpr = cloneExpr(*method.keyExpr, origins);
    res.isStatic = method.isStatic;
    res.isConstructor = method.isConstructor;
    res.isField = method.isField;
    res.isStaticBlock = method.isStaticBlock;
    if (method.init) res.init = cloneExpr(*method.init, origins);
    res.accessor = method.accessor;
    if (method.fn) {
        auto clonedFn = cloneExpr(*method.fn, origins);
        res.fn.reset(static_cast<FunctionExpr*>(clonedFn.release()));
    }
    return res;
}

namespace {

ExprPtr cloneExprNode(const Expr& expr, CloneOrigins* origins) {
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
        if (sp->argument) res->argument = cloneExpr(*sp->argument, origins);
        return res;
    }
    if (const auto* tl = dynamic_cast<const TemplateLit*>(&expr)) {
        auto res = std::make_unique<TemplateLit>();
        res->span = tl->span;
        res->parenthesized = tl->parenthesized;
        res->quasis = tl->quasis;
        res->rawQuasis = tl->rawQuasis;
        for (const auto& e : tl->exprs) res->exprs.push_back(cloneExpr(*e, origins));
        return res;
    }
    if (const auto* tt = dynamic_cast<const TaggedTemplate*>(&expr)) {
        auto res = std::make_unique<TaggedTemplate>();
        res->span = tt->span;
        res->parenthesized = tt->parenthesized;
        if (tt->tag) res->tag = cloneExpr(*tt->tag, origins);
        if (tt->templateLit) {
            auto clonedTpl = cloneExpr(*tt->templateLit, origins);
            res->templateLit.reset(static_cast<TemplateLit*>(clonedTpl.release()));
        }
        return res;
    }
    if (const auto* u = dynamic_cast<const Unary*>(&expr)) {
        auto res = std::make_unique<Unary>();
        res->span = u->span;
        res->parenthesized = u->parenthesized;
        res->op = u->op;
        if (u->operand) res->operand = cloneExpr(*u->operand, origins);
        return res;
    }
    if (const auto* bin = dynamic_cast<const Binary*>(&expr)) {
        auto res = std::make_unique<Binary>();
        res->span = bin->span;
        res->parenthesized = bin->parenthesized;
        res->op = bin->op;
        if (bin->lhs) res->lhs = cloneExpr(*bin->lhs, origins);
        if (bin->rhs) res->rhs = cloneExpr(*bin->rhs, origins);
        return res;
    }
    if (const auto* ter = dynamic_cast<const Ternary*>(&expr)) {
        auto res = std::make_unique<Ternary>();
        res->span = ter->span;
        res->parenthesized = ter->parenthesized;
        if (ter->condition) res->condition = cloneExpr(*ter->condition, origins);
        if (ter->thenExpr) res->thenExpr = cloneExpr(*ter->thenExpr, origins);
        if (ter->elseExpr) res->elseExpr = cloneExpr(*ter->elseExpr, origins);
        return res;
    }
    if (const auto* mem = dynamic_cast<const MemberAccess*>(&expr)) {
        auto res = std::make_unique<MemberAccess>();
        res->span = mem->span;
        res->parenthesized = mem->parenthesized;
        if (mem->object) res->object = cloneExpr(*mem->object, origins);
        res->property = mem->property;
        res->optional = mem->optional;
        res->isPrivate = mem->isPrivate;
        res->isPrivateDefine = mem->isPrivateDefine;
        return res;
    }
    if (const auto* idx = dynamic_cast<const IndexAccess*>(&expr)) {
        auto res = std::make_unique<IndexAccess>();
        res->span = idx->span;
        res->parenthesized = idx->parenthesized;
        if (idx->object) res->object = cloneExpr(*idx->object, origins);
        if (idx->index) res->index = cloneExpr(*idx->index, origins);
        res->optional = idx->optional;
        return res;
    }
    if (const auto* c = dynamic_cast<const Call*>(&expr)) {
        auto res = std::make_unique<Call>();
        res->span = c->span;
        res->parenthesized = c->parenthesized;
        if (c->callee) res->callee = cloneExpr(*c->callee, origins);
        for (const auto& a : c->args) res->args.push_back(cloneExpr(*a, origins));
        res->optional = c->optional;
        return res;
    }
    if (const auto* ne = dynamic_cast<const NewExpr*>(&expr)) {
        auto res = std::make_unique<NewExpr>();
        res->span = ne->span;
        res->parenthesized = ne->parenthesized;
        if (ne->callee) res->callee = cloneExpr(*ne->callee, origins);
        for (const auto& a : ne->args) res->args.push_back(cloneExpr(*a, origins));
        return res;
    }
    if (const auto* nt = dynamic_cast<const NewTargetExpr*>(&expr)) {
        auto res = std::make_unique<NewTargetExpr>();
        res->span = nt->span;
        res->parenthesized = nt->parenthesized;
        return res;
    }
    if (const auto* im = dynamic_cast<const ImportMetaExpr*>(&expr)) {
        auto res = std::make_unique<ImportMetaExpr>();
        res->span = im->span;
        res->parenthesized = im->parenthesized;
        return res;
    }
    if (const auto* sc = dynamic_cast<const SuperCall*>(&expr)) {
        auto res = std::make_unique<SuperCall>();
        res->span = sc->span;
        res->parenthesized = sc->parenthesized;
        res->baseName = sc->baseName;
        if (sc->baseExpr) res->baseExpr = cloneExpr(*sc->baseExpr, origins);
        for (const auto& a : sc->args) res->args.push_back(cloneExpr(*a, origins));
        return res;
    }
    if (const auto* sm = dynamic_cast<const SuperMember*>(&expr)) {
        auto res = std::make_unique<SuperMember>();
        res->span = sm->span;
        res->parenthesized = sm->parenthesized;
        res->baseName = sm->baseName;
        if (sm->baseExpr) res->baseExpr = cloneExpr(*sm->baseExpr, origins);
        res->property = sm->property;
        return res;
    }
    if (const auto* y = dynamic_cast<const YieldExpr*>(&expr)) {
        auto res = std::make_unique<YieldExpr>();
        res->span = y->span;
        res->parenthesized = y->parenthesized;
        if (y->argument) res->argument = cloneExpr(*y->argument, origins);
        res->delegate = y->delegate;
        res->isAwait = y->isAwait;
        return res;
    }
    if (const auto* d = dynamic_cast<const DestructuringAssign*>(&expr)) {
        auto res = std::make_unique<DestructuringAssign>();
        res->span = d->span;
        res->parenthesized = d->parenthesized;
        if (d->pattern) res->pattern = clonePattern(*d->pattern, origins);
        if (d->value) res->value = cloneExpr(*d->value, origins);
        return res;
    }
    if (const auto* di = dynamic_cast<const DynamicImportExpr*>(&expr)) {
        auto res = std::make_unique<DynamicImportExpr>();
        res->span = di->span;
        res->parenthesized = di->parenthesized;
        if (di->specifier) res->specifier = cloneExpr(*di->specifier, origins);
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
            if (p.keyExpr) prop.keyExpr = cloneExpr(*p.keyExpr, origins);
            if (p.value) prop.value = cloneExpr(*p.value, origins);
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
            res->elements.push_back(elem ? cloneExpr(*elem, origins) : nullptr);
        }
        return res;
    }
    if (const auto* fn = dynamic_cast<const FunctionExpr*>(&expr)) {
        auto res = std::make_unique<FunctionExpr>();
        res->span = fn->span;
        res->parenthesized = fn->parenthesized;
        res->name = fn->name;
        for (const auto& p : fn->params) res->params.push_back(cloneParam(p, origins));
        res->returnType = fn->returnType;
        for (const auto& s : fn->body) res->body.push_back(cloneStmt(*s, origins));
        res->isArrow = fn->isArrow;
        res->strict = fn->strict;
        res->isGenerator = fn->isGenerator;
        res->isAsync = fn->isAsync;
        res->kind = fn->kind;
        return res;
    }
    if (const auto* ce = dynamic_cast<const ClassExpr*>(&expr)) {
        auto res = std::make_unique<ClassExpr>();
        res->span = ce->span;
        res->parenthesized = ce->parenthesized;
        res->name = ce->name;
        res->superName = ce->superName;
        if (ce->superClass) res->superClass = cloneExpr(*ce->superClass, origins);
        for (const auto& m : ce->methods) res->methods.push_back(cloneClassMethod(m, origins));
        return res;
    }
    return nullptr;
}

StmtPtr cloneStmtNode(const Stmt& stmt, CloneOrigins* origins) {
    if (const auto* b = dynamic_cast<const BlockStmt*>(&stmt)) {
        auto res = std::make_unique<BlockStmt>();
        res->span = b->span;
        for (const auto& s : b->stmts) res->stmts.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* v = dynamic_cast<const VarDecl*>(&stmt)) {
        auto res = std::make_unique<VarDecl>();
        res->span = v->span;
        res->isConst = v->isConst;
        res->isVar = v->isVar;
        res->name = v->name;
        if (v->pattern) res->pattern = clonePattern(*v->pattern, origins);
        res->typeAnnotation = v->typeAnnotation;
        if (v->init) res->init = cloneExpr(*v->init, origins);
        return res;
    }
    if (const auto* r = dynamic_cast<const ReturnStmt*>(&stmt)) {
        auto res = std::make_unique<ReturnStmt>();
        res->span = r->span;
        if (r->value) res->value = cloneExpr(*r->value, origins);
        return res;
    }
    if (const auto* e = dynamic_cast<const ExprStmt*>(&stmt)) {
        auto res = std::make_unique<ExprStmt>();
        res->span = e->span;
        if (e->expr) res->expr = cloneExpr(*e->expr, origins);
        return res;
    }
    if (const auto* ifs = dynamic_cast<const IfStmt*>(&stmt)) {
        auto res = std::make_unique<IfStmt>();
        res->span = ifs->span;
        if (ifs->condition) res->condition = cloneExpr(*ifs->condition, origins);
        for (const auto& s : ifs->thenBody) res->thenBody.push_back(cloneStmt(*s, origins));
        for (const auto& s : ifs->elseBody) res->elseBody.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(&stmt)) {
        auto res = std::make_unique<WhileStmt>();
        res->span = w->span;
        if (w->condition) res->condition = cloneExpr(*w->condition, origins);
        for (const auto& s : w->body) res->body.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* dw = dynamic_cast<const DoWhileStmt*>(&stmt)) {
        auto res = std::make_unique<DoWhileStmt>();
        res->span = dw->span;
        for (const auto& s : dw->body) res->body.push_back(cloneStmt(*s, origins));
        if (dw->condition) res->condition = cloneExpr(*dw->condition, origins);
        return res;
    }
    if (const auto* f = dynamic_cast<const ForStmt*>(&stmt)) {
        auto res = std::make_unique<ForStmt>();
        res->span = f->span;
        for (const auto& s : f->init) res->init.push_back(cloneStmt(*s, origins));
        if (f->condition) res->condition = cloneExpr(*f->condition, origins);
        if (f->update) res->update = cloneExpr(*f->update, origins);
        for (const auto& s : f->body) res->body.push_back(cloneStmt(*s, origins));
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
        if (sw->discriminant) res->discriminant = cloneExpr(*sw->discriminant, origins);
        for (const auto& c : sw->cases) {
            SwitchCase sc;
            if (c.test) sc.test = cloneExpr(*c.test, origins);
            for (const auto& s : c.body) sc.body.push_back(cloneStmt(*s, origins));
            sc.span = c.span;
            res->cases.push_back(std::move(sc));
        }
        return res;
    }
    if (const auto* fi = dynamic_cast<const ForInStmt*>(&stmt)) {
        auto res = std::make_unique<ForInStmt>();
        res->span = fi->span;
        res->name = fi->name;
        if (fi->pattern) res->pattern = clonePattern(*fi->pattern, origins);
        res->isConst = fi->isConst;
        res->isLet = fi->isLet;
        res->isVar = fi->isVar;
        if (fi->object) res->object = cloneExpr(*fi->object, origins);
        for (const auto& s : fi->body) res->body.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* fo = dynamic_cast<const ForOfStmt*>(&stmt)) {
        auto res = std::make_unique<ForOfStmt>();
        res->span = fo->span;
        res->name = fo->name;
        if (fo->pattern) res->pattern = clonePattern(*fo->pattern, origins);
        res->isConst = fo->isConst;
        res->isLet = fo->isLet;
        res->isVar = fo->isVar;
        res->isAwait = fo->isAwait;
        if (fo->iterable) res->iterable = cloneExpr(*fo->iterable, origins);
        for (const auto& s : fo->body) res->body.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* lab = dynamic_cast<const LabeledStmt*>(&stmt)) {
        auto res = std::make_unique<LabeledStmt>();
        res->span = lab->span;
        res->label = lab->label;
        if (lab->body) res->body = cloneStmt(*lab->body, origins);
        return res;
    }
    if (const auto* t = dynamic_cast<const TryStmt*>(&stmt)) {
        auto res = std::make_unique<TryStmt>();
        res->span = t->span;
        for (const auto& s : t->body) res->body.push_back(cloneStmt(*s, origins));
        res->hasCatch = t->hasCatch;
        res->hasCatchParam = t->hasCatchParam;
        res->catchName = t->catchName;
        if (t->catchPattern) res->catchPattern = clonePattern(*t->catchPattern, origins);
        for (const auto& s : t->catchBody) res->catchBody.push_back(cloneStmt(*s, origins));
        res->hasFinally = t->hasFinally;
        for (const auto& s : t->finallyBody) res->finallyBody.push_back(cloneStmt(*s, origins));
        return res;
    }
    if (const auto* th = dynamic_cast<const ThrowStmt*>(&stmt)) {
        auto res = std::make_unique<ThrowStmt>();
        res->span = th->span;
        if (th->value) res->value = cloneExpr(*th->value, origins);
        return res;
    }
    if (const auto* cd = dynamic_cast<const ClassDecl*>(&stmt)) {
        auto res = std::make_unique<ClassDecl>();
        res->span = cd->span;
        res->name = cd->name;
        res->superName = cd->superName;
        if (cd->superClass) res->superClass = cloneExpr(*cd->superClass, origins);
        for (const auto& m : cd->methods) res->methods.push_back(cloneClassMethod(m, origins));
        return res;
    }
    if (const auto* fd = dynamic_cast<const FunctionDecl*>(&stmt)) {
        auto res = std::make_unique<FunctionDecl>();
        res->span = fd->span;
        res->isExported = fd->isExported;
        res->name = fd->name;
        for (const auto& p : fd->params) res->params.push_back(cloneParam(p, origins));
        res->returnType = fd->returnType;
        for (const auto& s : fd->body) res->body.push_back(cloneStmt(*s, origins));
        res->strict = fd->strict;
        res->isGenerator = fd->isGenerator;
        res->isAsync = fd->isAsync;
        return res;
    }
    return nullptr;
}

}  // namespace

// The two public entry points. Each delegates to the node builder above and
// then records ONE pairing — the recursion runs through here, so every node of
// the copied subtree is recorded, not only its root.
ExprPtr cloneExpr(const Expr& expr, CloneOrigins* origins) {
    ExprPtr res = cloneExprNode(expr, origins);
    if (origins != nullptr && res) origins->emplace(res.get(), &expr);
    return res;
}

StmtPtr cloneStmt(const Stmt& stmt, CloneOrigins* origins) {
    StmtPtr res = cloneStmtNode(stmt, origins);
    if (origins != nullptr && res) origins->emplace(res.get(), &stmt);
    return res;
}

}  // namespace bronze::ast
