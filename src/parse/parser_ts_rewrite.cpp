// The scope-aware rewrite walk the TypeScript lowerings share (see the
// header). Its traversal is the module linker's renamer, node for node: the
// two must agree about which scopes a name can be shadowed in, and the
// linker's is the one the whole test suite already exercises.

#include "parse/parser_ts_rewrite.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <typeinfo>

#include "ast/queries.h"

namespace bronze::ts_rewrite {

using namespace ast;

bool ScopedWalk::shadowed(const std::string& name) const {
    for (const auto& scope : shadow_) {
        if (scope.count(name)) return true;
    }
    return false;
}

void ScopedWalk::fail(Span span, const std::string& message) {
    if (!ok_) return;
    ok_ = false;
    diags_.error(span, message);
}

void ScopedWalk::stmtList(std::vector<StmtPtr>& list) {
    for (auto& s : list) {
        if (s && ok_) stmt(*s);
    }
}

// A statement list that IS a scope: a block, a loop body, an arm of an `if`.
void ScopedWalk::blockList(std::vector<StmtPtr>& list) {
    std::set<std::string> scope;
    for (const auto& name : getScopeDeclarations(list)) scope.insert(name);
    shadow_.push_back(std::move(scope));
    stmtList(list);
    shadow_.pop_back();
}

// A function's scope: its parameters, its own lexical declarations, every
// `var` hoisted to it, and a named function expression's own name.
void ScopedWalk::functionBody(std::vector<Param>& params, std::vector<StmtPtr>& body,
                              const std::string& ownName) {
    std::set<std::string> scope;
    if (!ownName.empty()) scope.insert(ownName);
    for (const auto& p : params) {
        if (p.pattern) {
            for (const auto& n : patternBoundNames(*p.pattern)) scope.insert(n);
        } else {
            scope.insert(p.name);
        }
    }
    for (const auto& n : getScopeDeclarations(body)) scope.insert(n);
    for (const auto& n : getHoistedVarDeclarations(body)) scope.insert(n);
    shadow_.push_back(std::move(scope));
    for (auto& p : params) {
        if (p.defaultValue) slot(p.defaultValue);
        if (p.pattern) pattern(*p.pattern, /*declaring=*/true);
    }
    stmtList(body);
    shadow_.pop_back();
}

// In a DECLARATION the enclosing scope has already pushed the names a pattern
// binds; in a destructuring ASSIGNMENT it has not, and each named element is a
// reference to a binding that already exists — which is what assignTarget is
// told about.
void ScopedWalk::pattern(BindingPattern& p, bool declaring) {
    for (auto& elem : p.elements) {
        if (!ok_) return;
        if (elem.keyExpr) slot(elem.keyExpr);
        if (elem.target) slot(elem.target);
        if (elem.defaultValue) slot(elem.defaultValue);
        if (elem.pattern) pattern(*elem.pattern, declaring);
        if (!declaring && !elem.name.empty()) assignTarget(elem);
    }
}

void ScopedWalk::stmt(Stmt& s) {
    if (auto* blk = dynamic_cast<BlockStmt*>(&s)) {
        blockList(blk->stmts);
    } else if (auto* vd = dynamic_cast<VarDecl*>(&s)) {
        if (vd->pattern) pattern(*vd->pattern, /*declaring=*/true);
        if (vd->init) slot(vd->init);
    } else if (auto* ret = dynamic_cast<ReturnStmt*>(&s)) {
        if (ret->value) slot(ret->value);
    } else if (auto* es = dynamic_cast<ExprStmt*>(&s)) {
        slot(es->expr);
    } else if (auto* ifs = dynamic_cast<IfStmt*>(&s)) {
        slot(ifs->condition);
        blockList(ifs->thenBody);
        blockList(ifs->elseBody);
    } else if (auto* wh = dynamic_cast<WhileStmt*>(&s)) {
        slot(wh->condition);
        blockList(wh->body);
    } else if (auto* dw = dynamic_cast<DoWhileStmt*>(&s)) {
        blockList(dw->body);
        slot(dw->condition);
    } else if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        std::set<std::string> scope;
        for (const auto& name : getScopeDeclarations(fs->init)) scope.insert(name);
        shadow_.push_back(std::move(scope));
        stmtList(fs->init);
        if (fs->condition) slot(fs->condition);
        if (fs->update) slot(fs->update);
        blockList(fs->body);
        shadow_.pop_back();
    } else if (auto* fi = dynamic_cast<ForInStmt*>(&s)) {
        slot(fi->object);
        iterationHead(fi->name, fi->pattern.get(), fi->isConst || fi->isLet || fi->isVar,
                      fi->body, fi->span);
    } else if (auto* fo = dynamic_cast<ForOfStmt*>(&s)) {
        slot(fo->iterable);
        iterationHead(fo->name, fo->pattern.get(), fo->isConst || fo->isLet || fo->isVar,
                      fo->body, fo->span);
    } else if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        slot(sw->discriminant);
        // One block scope for the whole switch body (ECMA-262 14.12).
        std::set<std::string> scope;
        for (const auto& c : sw->cases) {
            for (const auto& name : getScopeDeclarations(c.body)) scope.insert(name);
        }
        shadow_.push_back(std::move(scope));
        for (auto& c : sw->cases) {
            if (c.test) slot(c.test);
            stmtList(c.body);
        }
        shadow_.pop_back();
    } else if (auto* lab = dynamic_cast<LabeledStmt*>(&s)) {
        if (lab->body) stmt(*lab->body);
    } else if (auto* tr = dynamic_cast<TryStmt*>(&s)) {
        blockList(tr->body);
        std::set<std::string> scope;
        if (tr->hasCatchParam) {
            if (tr->catchPattern) {
                for (const auto& b : patternBoundNames(*tr->catchPattern)) scope.insert(b);
            } else {
                scope.insert(tr->catchName);
            }
        }
        for (const auto& name : getScopeDeclarations(tr->catchBody)) scope.insert(name);
        shadow_.push_back(std::move(scope));
        if (tr->catchPattern) pattern(*tr->catchPattern, /*declaring=*/true);
        stmtList(tr->catchBody);
        shadow_.pop_back();
        blockList(tr->finallyBody);
    } else if (auto* th = dynamic_cast<ThrowStmt*>(&s)) {
        slot(th->value);
    } else if (auto* cd = dynamic_cast<ClassDecl*>(&s)) {
        classBody(cd->name, cd->superClass, cd->superName, cd->methods);
    } else if (auto* fd = dynamic_cast<FunctionDecl*>(&s)) {
        if (enterFunction(nullptr, /*isArrow=*/false)) {
            functionBody(fd->params, fd->body, std::string());
            exitFunction(/*isArrow=*/false);
        }
    } else if (dynamic_cast<BreakStmt*>(&s) || dynamic_cast<ContinueStmt*>(&s) ||
               dynamic_cast<DebuggerStmt*>(&s) || dynamic_cast<ImportDecl*>(&s) ||
               dynamic_cast<ExportNamesDecl*>(&s)) {
        // No expression and no binding below them.
    } else {
        fail(s.span, std::string("internal error: the TypeScript rewrite cannot walk a ") +
                         typeid(s).name() + " node");
    }
}

// With a declaration keyword the head BINDS its target; without one it
// assigns a binding that already exists, which is a reference.
void ScopedWalk::iterationHead(const std::string& name, BindingPattern* pat, bool declares,
                               std::vector<StmtPtr>& body, Span span) {
    std::set<std::string> scope;
    if (declares) {
        if (pat) {
            for (const auto& b : patternBoundNames(*pat)) scope.insert(b);
        } else {
            scope.insert(name);
        }
    } else if (!name.empty()) {
        iterationTarget(name, span);
    }
    shadow_.push_back(std::move(scope));
    if (pat) pattern(*pat, declares);
    blockList(body);
    shadow_.pop_back();
}

// The heritage and computed keys are evaluated where the class is; the
// members' bodies see the class's own name as a binding (ECMA-262 15.7.14
// step 4, the inner class scope).
void ScopedWalk::classBody(const std::string& ownName, ExprPtr& superClass,
                           std::string& superName, std::vector<ClassMethod>& methods) {
    if (superClass) {
        slot(superClass);
        const std::string renamed = heritageName(superClass.get());
        if (!renamed.empty()) superName = renamed;
    }
    if (!enterClassBody()) return;
    std::set<std::string> scope;
    if (!ownName.empty()) scope.insert(ownName);
    shadow_.push_back(std::move(scope));
    for (auto& m : methods) {
        if (!ok_) break;
        if (m.keyExpr) slot(m.keyExpr);
        if (m.init) slot(m.init);
        if (!m.fn) continue;
        if (enterFunction(m.fn.get(), /*isArrow=*/false)) {
            functionBody(m.fn->params, m.fn->body, std::string());
            exitFunction(/*isArrow=*/false);
        }
    }
    shadow_.pop_back();
}

void ScopedWalk::slot(ExprPtr& p) {
    if (!p || !ok_) return;
    if (visitSlot(p)) return;
    expr(*p);
}

void ScopedWalk::expr(Expr& e) {
    if (dynamic_cast<Ident*>(&e) || dynamic_cast<NumberLit*>(&e) ||
        dynamic_cast<BigIntLit*>(&e) || dynamic_cast<StringLit*>(&e) ||
        dynamic_cast<BoolLit*>(&e) || dynamic_cast<NullLit*>(&e) ||
        dynamic_cast<UndefinedLit*>(&e) || dynamic_cast<ThisExpr*>(&e) ||
        dynamic_cast<RegExpLit*>(&e) || dynamic_cast<NewTargetExpr*>(&e) ||
        dynamic_cast<ImportMetaExpr*>(&e)) {
        // A leaf: an Ident is visitSlot's to decide.
    } else if (auto* tpl = dynamic_cast<TemplateLit*>(&e)) {
        for (auto& sub : tpl->exprs) slot(sub);
    } else if (auto* un = dynamic_cast<Unary*>(&e)) {
        slot(un->operand);
    } else if (auto* bin = dynamic_cast<Binary*>(&e)) {
        slot(bin->lhs);
        slot(bin->rhs);
    } else if (auto* ter = dynamic_cast<Ternary*>(&e)) {
        slot(ter->condition);
        slot(ter->thenExpr);
        slot(ter->elseExpr);
    } else if (auto* mem = dynamic_cast<MemberAccess*>(&e)) {
        slot(mem->object);  // `property` is a key, never a binding
    } else if (auto* idx = dynamic_cast<IndexAccess*>(&e)) {
        slot(idx->object);
        slot(idx->index);
    } else if (auto* call = dynamic_cast<Call*>(&e)) {
        slot(call->callee);
        for (auto& arg : call->args) slot(arg);
    } else if (auto* nw = dynamic_cast<NewExpr*>(&e)) {
        slot(nw->callee);
        for (auto& arg : nw->args) slot(arg);
    } else if (auto* sc = dynamic_cast<SuperCall*>(&e)) {
        if (sc->baseExpr) {
            slot(sc->baseExpr);
            const std::string renamed = heritageName(sc->baseExpr.get());
            if (!renamed.empty()) sc->baseName = renamed;
        }
        for (auto& arg : sc->args) slot(arg);
    } else if (auto* sm = dynamic_cast<SuperMember*>(&e)) {
        if (sm->baseExpr) {
            slot(sm->baseExpr);
            const std::string renamed = heritageName(sm->baseExpr.get());
            if (!renamed.empty() && !sm->fromObjectLiteral) sm->baseName = renamed;
        }
        if (sm->propertyExpr) slot(sm->propertyExpr);
    } else if (auto* spr = dynamic_cast<SpreadElement*>(&e)) {
        slot(spr->argument);
    } else if (auto* y = dynamic_cast<YieldExpr*>(&e)) {
        slot(y->argument);
    } else if (auto* di = dynamic_cast<DynamicImportExpr*>(&e)) {
        if (di->specifier) slot(di->specifier);
    } else if (auto* da = dynamic_cast<DestructuringAssign*>(&e)) {
        pattern(*da->pattern, /*declaring=*/false);
        slot(da->value);
    } else if (auto* obj = dynamic_cast<ObjectLit*>(&e)) {
        for (auto& prop : obj->props) {
            if (prop.keyExpr) slot(prop.keyExpr);
            if (prop.value) slot(prop.value);
        }
    } else if (auto* arr = dynamic_cast<ArrayLit*>(&e)) {
        for (auto& elem : arr->elements) {
            if (elem) slot(elem);
        }
    } else if (auto* fe = dynamic_cast<FunctionExpr*>(&e)) {
        if (enterFunction(fe, fe->isArrow)) {
            functionBody(fe->params, fe->body, fe->isArrow ? std::string() : fe->name);
            exitFunction(fe->isArrow);
        }
    } else if (auto* ce = dynamic_cast<ClassExpr*>(&e)) {
        classBody(ce->name, ce->superClass, ce->superName, ce->methods);
    } else if (auto* tt = dynamic_cast<TaggedTemplate*>(&e)) {
        slot(tt->tag);
        for (auto& exp : tt->templateLit->exprs) slot(exp);
    } else {
        fail(e.span, std::string("internal error: the TypeScript rewrite cannot walk a ") +
                         typeid(e).name() + " node");
    }
}

ExprPtr QualifyNames::member(const std::string& name, Span span) const {
    auto object = std::make_unique<Ident>();
    object->span = span;
    object->name = object_;
    auto mem = std::make_unique<MemberAccess>();
    mem->span = span;
    mem->object = std::move(object);
    mem->property = name;
    return mem;
}

bool QualifyNames::visitSlot(ExprPtr& p) {
    auto* id = dynamic_cast<Ident*>(p.get());
    if (!id || !qualifies(id->name)) return false;
    p = member(id->name, id->span);
    return true;
}

void QualifyNames::assignTarget(PatternElement& elem) {
    if (!qualifies(elem.name)) return;
    elem.target = member(elem.name, elem.span);
    elem.name.clear();
}

void QualifyNames::iterationTarget(const std::string& name, Span span) {
    if (!qualifies(name)) return;
    fail(span, "unsupported construct: a 'for' head that assigns '" + name +
                   "', which is exported from its namespace; declare the loop variable and "
                   "assign the export inside the body");
}

ExprPtr makeIdent(const std::string& name, Span span) {
    auto id = std::make_unique<Ident>();
    id->span = span;
    id->name = name;
    return id;
}

ExprPtr makeString(const std::string& value, Span span) {
    auto s = std::make_unique<StringLit>();
    s->span = span;
    s->value = value;
    return s;
}

ExprPtr makeNumber(double value, Span span) {
    auto n = std::make_unique<NumberLit>();
    n->span = span;
    n->value = value;
    return n;
}

ExprPtr makeMember(ExprPtr object, const std::string& property, Span span) {
    auto mem = std::make_unique<MemberAccess>();
    mem->span = span;
    mem->object = std::move(object);
    mem->property = property;
    return mem;
}

ExprPtr makeIndex(ExprPtr object, ExprPtr index, Span span) {
    auto idx = std::make_unique<IndexAccess>();
    idx->span = span;
    idx->object = std::move(object);
    idx->index = std::move(index);
    return idx;
}

ExprPtr makeBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs, Span span) {
    auto bin = std::make_unique<Binary>();
    bin->span = span;
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    return bin;
}

StmtPtr makeExprStmt(ExprPtr expr, Span span) {
    auto es = std::make_unique<ExprStmt>();
    es->span = span;
    es->expr = std::move(expr);
    return es;
}

ExprPtr makeIife(const std::string& param, std::vector<StmtPtr> body, ExprPtr arg, bool strict,
                 Span span) {
    auto fn = std::make_unique<FunctionExpr>();
    fn->span = span;
    fn->kind = FunctionKind::Normal;
    fn->strict = strict;
    Param p;
    p.name = param;
    p.span = span;
    fn->params.push_back(std::move(p));
    fn->body = std::move(body);
    auto call = std::make_unique<Call>();
    call->span = span;
    call->callee = std::move(fn);
    call->args.push_back(std::move(arg));
    return call;
}

std::string numberToString(double v) {
    if (std::isnan(v)) return "NaN";
    if (v == 0.0) return "0";  // -0 included (step 2)
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    const std::string sign = v < 0 ? "-" : "";
    // The shortest digits that round-trip are exactly the k digits of s that
    // step 5 asks for; to_chars produces them, in scientific form.
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), std::fabs(v),
                                   std::chars_format::scientific);
    const std::string sci(buf, res.ptr);
    const size_t ePos = sci.find('e');
    std::string digits;
    for (size_t i = 0; i < ePos; ++i) {
        if (sci[i] != '.') digits += sci[i];
    }
    const int k = static_cast<int>(digits.size());
    const int n = std::atoi(sci.c_str() + ePos + 1) + 1;
    if (k <= n && n <= 21) return sign + digits + std::string(static_cast<size_t>(n - k), '0');
    if (0 < n && n <= 21) {
        return sign + digits.substr(0, static_cast<size_t>(n)) + "." +
               digits.substr(static_cast<size_t>(n));
    }
    if (-6 < n && n <= 0) return sign + "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    const std::string exponent =
        std::string(n - 1 >= 0 ? "+" : "-") + std::to_string(n - 1 >= 0 ? n - 1 : 1 - n);
    if (k == 1) return sign + digits + "e" + exponent;
    return sign + digits.substr(0, 1) + "." + digits.substr(1) + "e" + exponent;
}

uint32_t toUint32(double v) {
    if (!std::isfinite(v)) return 0;
    double m = std::fmod(std::trunc(v), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<uint32_t>(m);
}

int32_t toInt32(double v) {
    return static_cast<int32_t>(toUint32(v));
}

std::string heritageName(const Expr* expr) {
    if (!expr) return "";
    if (const auto* id = dynamic_cast<const Ident*>(expr)) return id->name;
    if (const auto* mem = dynamic_cast<const MemberAccess*>(expr)) {
        const std::string prefix = heritageName(mem->object.get());
        return prefix.empty() ? mem->property : prefix + "." + mem->property;
    }
    return "";
}

}  // namespace bronze::ts_rewrite
