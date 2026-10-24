// The rewrite patterns of `rewriteJsSource` (#143), found as nodes of the
// parsed program (#162). Each pattern names the expression shape it reads, the
// facts that make its replacement equal to the original wherever it fires,
// and the replacement it splices in. What none of them can see — the types of
// a function's parameters — is what the SMT gate in js_source_rewriter.cpp
// checks the rewritten function against, over its int32 domain.

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "check/js_rewrite_ast.h"

namespace bronze::check::jsrw {

using namespace bronze::ast;

namespace {

std::optional<double> literalNumber(const Expr* e) {
    if (auto* n = exactAs<const NumberLit*>(e)) return n->value;
    if (auto* u = exactAs<const Unary*>(e)) {
        if (u->op == UnaryOp::Negate) {
            if (auto* n = exactAs<const NumberLit*>(u->operand.get())) return -n->value;
        }
    }
    return std::nullopt;
}

// k when `v` is exactly 2^k with 1 <= k <= maxShift.
int log2IfPow2(double v, int maxShift) {
    for (int k = 1; k <= maxShift; ++k) {
        if (v == std::ldexp(1.0, k)) return k;
    }
    return -1;
}

std::string fmtNumber(double v) {
    if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
        return std::to_string(static_cast<long long>(v));
    }
    std::string s = std::to_string(v);
    return s;
}

const Ident* asIdent(const Expr* e) { return exactAs<const Ident*>(e); }

bool sameIdent(const Expr* e, const std::string& name) {
    auto* id = asIdent(e);
    return id && id->name == name;
}

Edit makeEdit(const ParsedJs& p, Extent at, std::string text, const Node& site,
              const std::string& fallbackFn, const char* rule, std::string description) {
    Edit e;
    e.at = at;
    e.text = p.asiSafe(at, std::move(text));
    std::string before = p.slice(at);
    std::string fn = p.functionNameOf(site);
    e.hit = {fn.empty() ? fallbackFn : fn,
             rule,
             std::move(description),
             before,
             e.text,
             true,
             fnv1aHex(before + "->" + e.text)};
    return e;
}

// Does this body declare anything that outlives it: a `var` (function
// scoped) or a function declaration (which sloppy-mode Annex B also hoists)?
// Removing such a body would remove the binding, not just dead code.
bool declaresHoisted(const std::vector<StmtPtr>& body) {
    if (!getHoistedVarDeclarations(body).empty()) return true;
    struct Finder : detail::IdentVisitor {
        bool found = false;
        void visit(const FunctionDecl&) override { found = true; }
        void visit(const FunctionExpr&) override {}  // its own scope
        void visit(const ClassExpr&) override {}
        void visit(const ClassDecl&) override {}
    } finder;
    for (const auto& s : body) {
        if (s) s->accept(finder);
    }
    return finder.found;
}

}  // namespace

// ---- Pass 1: guards a value's known range makes redundant ------------------

void collectDeadGuardEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    // Math.max(0, E) / Math.max(E, 0) -> (E) when E is a non-negative integer;
    // Math.min(H, E) / Math.min(E, H) -> (E) when E is an integer <= H. E is
    // evaluated exactly once either way, and an integer is never NaN or -0.
    for (const Call* c : p.calls()) {
        const bool isMax = p.isMathCall(*c, "max");
        const bool isMin = !isMax && p.isMathCall(*c, "min");
        if ((!isMax && !isMin) || c->args.size() != 2) continue;
        for (int litSide = 0; litSide < 2; ++litSide) {
            auto lit = literalNumber(c->args[litSide].get());
            const Expr& e = *c->args[1 - litSide];
            if (!lit || !std::isfinite(*lit)) continue;
            auto r = p.rangeOf(e);
            if (!r) continue;
            if (isMax && *lit == 0 && r->lo >= 0) {
                out.push_back(makeEdit(p, p.inner(*c), "(" + p.exprText(e) + ")", *c, fallbackFn,
                                       "smt-dead-guard-elim",
                                       "Stripped redundant Math.max(0, ...) on a value proven to be a "
                                       "non-negative integer in [" + fmtNumber(r->lo) + ", " +
                                           fmtNumber(r->hi) + "]"));
                break;
            }
            if (isMin && r->hi <= *lit) {
                out.push_back(makeEdit(p, p.inner(*c), "(" + p.exprText(e) + ")", *c, fallbackFn,
                                       "smt-dead-guard-elim",
                                       "Stripped redundant Math.min(" + fmtNumber(*lit) +
                                           ", ...) on a value proven to be an integer in [" +
                                           fmtNumber(r->lo) + ", " + fmtNumber(r->hi) + "]"));
                break;
            }
        }
    }

    // `if (v < L) ...` (and <=, >, >=, either way round) with no else, where v
    // is a binding whose known range makes the test false: the branch can
    // never run, so the whole statement goes.
    for (const IfStmt* s : p.ifs()) {
        if (p.hasElse(*s)) continue;
        auto* cond = exactAs<const Binary*>(s->condition.get());
        if (!cond) continue;
        BinaryOp op = cond->op;
        if (op != BinaryOp::Less && op != BinaryOp::LessEqual && op != BinaryOp::Greater &&
            op != BinaryOp::GreaterEqual) {
            continue;
        }
        const Ident* v = asIdent(cond->lhs.get());
        auto lit = literalNumber(cond->rhs.get());
        if (!v || !lit) {
            // `L < v` is `v > L`.
            v = asIdent(cond->rhs.get());
            lit = literalNumber(cond->lhs.get());
            if (!v || !lit) continue;
            switch (op) {
                case BinaryOp::Less: op = BinaryOp::Greater; break;
                case BinaryOp::LessEqual: op = BinaryOp::GreaterEqual; break;
                case BinaryOp::Greater: op = BinaryOp::Less; break;
                default: op = BinaryOp::LessEqual; break;
            }
        }
        if (!std::isfinite(*lit)) continue;
        auto r = p.rangeOf(*v);
        if (!r) continue;
        bool neverTrue = false;
        switch (op) {
            case BinaryOp::Less: neverTrue = r->lo >= *lit; break;
            case BinaryOp::LessEqual: neverTrue = r->lo > *lit; break;
            case BinaryOp::Greater: neverTrue = r->hi <= *lit; break;
            default: neverTrue = r->hi < *lit; break;
        }
        if (!neverTrue || declaresHoisted(s->thenBody)) continue;
        auto ext = p.statement(*s);
        if (!ext) continue;
        Edit e = deletionEdit(p, *ext);
        std::string before = p.slice(*ext);
        std::string fn = p.functionNameOf(*s);
        e.hit = {fn.empty() ? fallbackFn : fn,
                 "smt-dead-branch-elim",
                 "Eliminated unreachable branch `if (" + p.exprText(*s->condition) + ")`: `" + v->name +
                     "` is proven to be an integer in [" + fmtNumber(r->lo) + ", " + fmtNumber(r->hi) + "]",
                 before,
                 "/* unreachable branch eliminated */",
                 true,
                 fnv1aHex(before)};
        out.push_back(std::move(e));
    }
}

// ---- Pass 2: strength reduction --------------------------------------------

void collectStrengthEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    for (const Call* c : p.calls()) {
        // Math.floor(E / 2^k) -> (E >> k) for an int32 E: an arithmetic shift
        // IS floor division by 2^k there. Math.trunc rounds toward zero, which
        // agrees with it only when E is non-negative.
        const bool isFloor = p.isMathCall(*c, "floor");
        const bool isTrunc = !isFloor && p.isMathCall(*c, "trunc");
        if ((isFloor || isTrunc) && c->args.size() == 1) {
            auto* div = exactAs<const Binary*>(c->args[0].get());
            if (div && div->op == BinaryOp::Div) {
                auto d = literalNumber(div->rhs.get());
                int sh = d ? log2IfPow2(*d, 30) : -1;
                auto r = p.rangeOf(*div->lhs);
                if (sh > 0 && r && r->isInt32() && (isFloor || r->lo >= 0)) {
                    out.push_back(makeEdit(
                        p, p.inner(*c), "(" + p.operandText(*div->lhs) + " >> " + std::to_string(sh) + ")",
                        *c, fallbackFn, "strength-reduction-shift",
                        "Reduced division by power-of-two (" + fmtNumber(*d) + ") to bit-shift >> " +
                            std::to_string(sh) + " on an int32 in [" + fmtNumber(r->lo) + ", " +
                            fmtNumber(r->hi) + "]"));
                }
            }
            continue;
        }

        // Math.pow(x, 2) -> (x * x): the square of a Number rounds once
        // either way. Math.pow(x, 3) -> (x * x * x) only where x is an integer
        // whose cube is exact, since x * x * x rounds twice.
        if (p.isMathCall(*c, "pow") && c->args.size() == 2) {
            const Ident* x = asIdent(c->args[0].get());
            auto e = literalNumber(c->args[1].get());
            if (!x || !e || (*e != 2 && *e != 3)) continue;
            if (*e == 3) {
                auto r = p.rangeOf(*x);
                double m = r ? std::max(std::fabs(r->lo), std::fabs(r->hi)) : INFINITY;
                if (!(m * m * m <= 9007199254740992.0)) continue;
            }
            const std::string& v = x->name;
            std::string after = (*e == 2) ? "(" + v + " * " + v + ")" : "(" + v + " * " + v + " * " + v + ")";
            out.push_back(makeEdit(p, p.inner(*c), after, *c, fallbackFn, "strength-reduction-pow",
                                   "Strength-reduced Math.pow(" + v + ", " + fmtNumber(*e) +
                                       ") to direct multiply"));
            continue;
        }

        // Math.imul(A, 2^k) -> (A << k): both are ToInt32(A) * 2^k modulo 2^32,
        // and A is evaluated once either way.
        if (p.isMathCall(*c, "imul") && c->args.size() == 2) {
            for (int litSide = 0; litSide < 2; ++litSide) {
                auto f = literalNumber(c->args[litSide].get());
                int sh = f ? log2IfPow2(*f, 31) : -1;
                if (sh <= 0) continue;
                const Expr& a = *c->args[1 - litSide];
                out.push_back(makeEdit(p, p.inner(*c), "(" + p.operandText(a) + " << " + std::to_string(sh) + ")",
                                       *c, fallbackFn, "strength-reduction-imul",
                                       "Reduced Math.imul by " + fmtNumber(*f) + " to left shift << " +
                                           std::to_string(sh)));
                break;
            }
        }
    }
}

// ---- Pass 3: Horner's rule -------------------------------------------------

namespace {

struct Power {
    std::string var;
    int degree = 0;
};

// x, x * x, x * x * x, Math.pow(x, 2|3).
std::optional<Power> powerOf(const ParsedJs& p, const Expr* e) {
    if (auto* id = asIdent(e)) return Power{id->name, 1};
    if (auto* c = exactAs<const Call*>(e)) {
        if (!p.isMathCall(*c, "pow") || c->args.size() != 2) return std::nullopt;
        auto* x = asIdent(c->args[0].get());
        auto k = literalNumber(c->args[1].get());
        if (!x || !k || (*k != 2 && *k != 3)) return std::nullopt;
        return Power{x->name, static_cast<int>(*k)};
    }
    auto* m = exactAs<const Binary*>(e);
    if (!m || m->op != BinaryOp::Mul) return std::nullopt;
    auto* rhs = asIdent(m->rhs.get());
    if (!rhs) return std::nullopt;
    auto l = powerOf(p, m->lhs.get());
    if (!l || l->var != rhs->name || l->degree >= 3) return std::nullopt;
    return Power{l->var, l->degree + 1};
}

// `[k *] x^d`, with k a non-negative integer literal (1 when absent).
std::optional<std::pair<double, Power>> termOf(const ParsedJs& p, const Expr* e) {
    if (auto* m = exactAs<const Binary*>(e)) {
        if (m->op == BinaryOp::Mul) {
            auto k = literalNumber(m->lhs.get());
            if (k) {
                if (*k < 0 || *k != std::floor(*k)) return std::nullopt;
                auto pw = powerOf(p, m->rhs.get());
                if (!pw) return std::nullopt;
                return std::make_pair(*k, *pw);
            }
        }
    }
    auto pw = powerOf(p, e);
    if (!pw) return std::nullopt;
    return std::make_pair(1.0, *pw);
}

}  // namespace

void collectHornerEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    // a*x^3 + b*x^2 + c*x + d  ->  ((a*x + b)*x + c)*x + d, for an integer x
    // small enough that every intermediate of both forms is an exact integer.
    for (const Binary* sum : p.binaries()) {
        if (sum->op != BinaryOp::Add) continue;
        auto d = literalNumber(sum->rhs.get());
        auto* s1 = exactAs<const Binary*>(sum->lhs.get());
        if (!d || *d < 0 || *d != std::floor(*d) || !s1 || s1->op != BinaryOp::Add) continue;
        auto* s2 = exactAs<const Binary*>(s1->lhs.get());
        if (!s2 || s2->op != BinaryOp::Add) continue;
        auto t3 = termOf(p, s2->lhs.get());
        auto t2 = termOf(p, s2->rhs.get());
        auto t1 = termOf(p, s1->rhs.get());
        if (!t3 || !t2 || !t1) continue;
        if (t3->second.degree != 3 || t2->second.degree != 2 || t1->second.degree != 1) continue;
        const std::string& x = t3->second.var;
        if (t2->second.var != x || t1->second.var != x) continue;
        // x must be one binding with a known range: the same name written
        // three times in one expression is one binding.
        const Ident* xNode = nullptr;
        for (const Ident* id : p.idents()) {
            Extent ie = p.inner(*id);
            Extent se = p.inner(*sum);
            if (id->name == x && ie.begin >= se.begin && ie.end <= se.end) {
                xNode = id;
                break;
            }
        }
        auto r = xNode ? p.rangeOf(*xNode) : std::nullopt;
        if (!r) continue;
        double m = std::max(std::fabs(r->lo), std::fabs(r->hi));
        double a = t3->first, b = t2->first, c = t1->first;
        if (!(a * m * m * m + b * m * m + c * m + *d <= 9007199254740992.0)) continue;
        std::string head = (a == 1) ? "(" + x + " + " + fmtNumber(b) + ")"
                                    : "(" + fmtNumber(a) + " * " + x + " + " + fmtNumber(b) + ")";
        std::string after = "((" + head + " * " + x + " + " + fmtNumber(c) + ") * " + x + " + " + fmtNumber(*d) + ")";
        out.push_back(makeEdit(p, p.inner(*sum), after, *sum, fallbackFn, "horner-polynomial",
                               "Factored degree-3 polynomial into Horner's nested form (6 multiplies -> "
                               "3 multiplies) over the integer " + x + " in [" + fmtNumber(r->lo) + ", " +
                                   fmtNumber(r->hi) + "]"));
    }
}

// ---- Pass 4: loops ---------------------------------------------------------

namespace {

// Operators over operands that are themselves such expressions: no call, no
// property read, no assignment, nothing that can run user code on primitive
// operands. `allowed`, when given, is the only names it may read.
bool pureOps(const Expr* e, const std::unordered_set<std::string>* allowed) {
    if (!e) return false;
    if (exactAs<const NumberLit*>(e) || exactAs<const StringLit*>(e) || exactAs<const BoolLit*>(e) ||
        exactAs<const NullLit*>(e) || exactAs<const UndefinedLit*>(e)) {
        return true;
    }
    if (auto* id = asIdent(e)) {
        if (allowed) return allowed->count(id->name) > 0;
        return id->name != "eval" && id->name != "arguments" && id->name.find('.') == std::string::npos;
    }
    if (auto* u = exactAs<const Unary*>(e)) {
        switch (u->op) {
            case UnaryOp::Not:
            case UnaryOp::Negate:
            case UnaryOp::Posate:
            case UnaryOp::BitNot:
            case UnaryOp::TypeOf:
                return pureOps(u->operand.get(), allowed);
            default:
                return false;
        }
    }
    if (auto* t = exactAs<const Ternary*>(e)) {
        return pureOps(t->condition.get(), allowed) && pureOps(t->thenExpr.get(), allowed) &&
               pureOps(t->elseExpr.get(), allowed);
    }
    if (auto* b = exactAs<const Binary*>(e)) {
        if (isAssignOp(b->op) || b->op == BinaryOp::In || b->op == BinaryOp::InstanceOf ||
            b->op == BinaryOp::Comma) {
            return false;
        }
        return pureOps(b->lhs.get(), allowed) && pureOps(b->rhs.get(), allowed);
    }
    return false;
}

// An operator whose result is always a primitive, whatever its operands.
bool primitiveResult(const Expr* e) {
    if (exactAs<const Unary*>(e)) return true;  // pureOps admitted only primitive-valued ones
    if (auto* b = exactAs<const Binary*>(e)) {
        return b->op != BinaryOp::LogicalAnd && b->op != BinaryOp::LogicalOr &&
               b->op != BinaryOp::NullishCoalescing;
    }
    return false;
}

bool isPrimitiveLiteral(const Expr* e) {
    return literalNumber(e) || exactAs<const StringLit*>(e) || exactAs<const BoolLit*>(e) ||
           exactAs<const NullLit*>(e) || exactAs<const UndefinedLit*>(e);
}

// `(p1, ..., pn) => expr` with plain parameter names: the expression.
const Expr* arrowBody(const Expr* e, size_t arity, std::vector<std::string>& params) {
    auto* f = exactAs<const FunctionExpr*>(e);
    if (!f || !f->isArrow || f->isAsync || f->isGenerator || f->params.size() != arity) return nullptr;
    for (const auto& prm : f->params) {
        if (prm.name.empty() || prm.pattern || prm.defaultValue || prm.isRest) return nullptr;
        params.push_back(prm.name);
    }
    if (f->body.size() != 1) return nullptr;
    auto* r = exactAs<const ReturnStmt*>(f->body[0].get());
    return r ? r->value.get() : nullptr;
}

// `<recv>.<method>(args...)`, non-optional and spread-free.
const Call* methodCall(const Expr* e, const char* method, size_t nargs, const Expr** receiver) {
    auto* c = exactAs<const Call*>(e);
    if (!c || c->optional || c->args.size() != nargs) return nullptr;
    auto* m = exactAs<const MemberAccess*>(c->callee.get());
    if (!m || m->optional || m->isPrivate || m->property != method) return nullptr;
    for (const auto& a : c->args) {
        if (exactAs<const SpreadElement*>(a.get())) return nullptr;
    }
    *receiver = m->object.get();
    return c;
}

void collectDeforestation(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    // return items.map(x => M).filter(y => F).reduce((acc, z) => R, INIT);
    //
    // Fused into one loop under a guard that `items` is a plain Array (the
    // original chain runs otherwise). The callbacks read nothing but
    // operators: M may read outer names, F and R only their parameters, so
    // what F and R see are M's primitive results and they cannot run user
    // code. Each callback's parameters are re-bound with `const` in a block of
    // their own, so no text inside a callback is rewritten.
    static const char* kFresh[] = {"__acc", "__i", "__n", "__m", "__keep"};
    for (const ReturnStmt* ret : p.returns()) {
        if (!ret->value) continue;
        const Expr* filtered = nullptr;
        const Expr* mapped = nullptr;
        const Expr* recv = nullptr;
        const Call* reduce = methodCall(ret->value.get(), "reduce", 2, &filtered);
        const Call* filter = reduce ? methodCall(filtered, "filter", 1, &mapped) : nullptr;
        const Call* map = filter ? methodCall(mapped, "map", 1, &recv) : nullptr;
        const Ident* items = map ? asIdent(recv) : nullptr;
        if (!items) continue;

        std::vector<std::string> mp, fp, rp;
        const Expr* mBody = arrowBody(map->args[0].get(), 1, mp);
        const Expr* fBody = arrowBody(filter->args[0].get(), 1, fp);
        const Expr* rBody = arrowBody(reduce->args[0].get(), 2, rp);
        const Expr* init = reduce->args[1].get();
        if (!mBody || !fBody || !rBody || rp[0] == rp[1] || !isPrimitiveLiteral(init)) continue;
        if (mp[0] == items->name || fp[0] == items->name || rp[0] == items->name || rp[1] == items->name) {
            continue;
        }
        std::unordered_set<std::string> fNames{fp[0]}, rNames{rp[0], rp[1]};
        if (!pureOps(mBody, nullptr) || !primitiveResult(mBody) || !pureOps(fBody, &fNames) ||
            !pureOps(rBody, &rNames)) {
            continue;
        }
        if (!p.isGlobal("Array") || !p.isGlobal("Object")) continue;
        bool fresh = true;
        for (const char* n : kFresh) fresh = fresh && p.isFreshName(n);
        if (!fresh) continue;
        auto ext = p.statement(*ret);
        if (!ext) continue;

        const std::string& a = items->name;
        // A braceless `if (c) return ...;` body must stay ONE statement, so
        // the loop is then wrapped in a block of its own.
        const Token* prev = p.tokenBefore(ext->begin);
        const bool listContext = !prev || prev->kind == TokenKind::Semicolon ||
                                 prev->kind == TokenKind::LBrace || prev->kind == TokenKind::RBrace;
        const std::string outerInd = p.indentAt(ext->begin);
        const std::string ind = listContext ? outerInd : outerInd + "    ";
        std::string body;
        body += "if (!Array.isArray(" + a + ") || Object.getPrototypeOf(" + a +
                ") !== Array.prototype) return " + p.exprText(*ret->value) + ";\n";
        body += ind + "let __acc = " + p.exprText(*init) + ";\n";
        body += ind + "for (let __i = 0, __n = " + a + ".length; __i < __n; ++__i) {\n";
        body += ind + "    if (!(__i in " + a + ")) continue;\n";
        body += ind + "    let __m;\n";
        body += ind + "    { const " + mp[0] + " = " + a + "[__i]; __m = (" + p.exprText(*mBody) + "); }\n";
        body += ind + "    let __keep;\n";
        body += ind + "    { const " + fp[0] + " = __m; __keep = (" + p.exprText(*fBody) + "); }\n";
        body += ind + "    if (__keep) { const " + rp[0] + " = __acc, " + rp[1] + " = __m; __acc = (" +
                p.exprText(*rBody) + "); }\n";
        body += ind + "}\n";
        body += ind + "return __acc;";
        if (!listContext) body = "{\n" + ind + body + "\n" + outerInd + "}";
        out.push_back(makeEdit(p, *ext, body, *ret, fallbackFn, "loop-deforestation",
                               "Fused .map().filter().reduce() over a plain Array into a single "
                               "zero-allocation loop (the chain runs as written for anything else)"));
    }
}

// `x++`, `++x`, `x += 1`, `x = x + 1`.
bool incrementsByOne(const Expr* e, const std::string& x) {
    if (auto* u = exactAs<const Unary*>(e)) {
        return (u->op == UnaryOp::PostInc || u->op == UnaryOp::PreInc) && sameIdent(u->operand.get(), x);
    }
    auto* b = exactAs<const Binary*>(e);
    if (!b || !sameIdent(b->lhs.get(), x)) return false;
    auto one = literalNumber(b->rhs.get());
    if (b->op == BinaryOp::PlusAssign) return one && *one == 1;
    if (b->op != BinaryOp::Assign) return false;
    auto* add = exactAs<const Binary*>(b->rhs.get());
    auto k = add ? literalNumber(add->rhs.get()) : std::nullopt;
    return add && add->op == BinaryOp::Add && sameIdent(add->lhs.get(), x) && k && *k == 1;
}

void collectClosedFormSeries(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    // let s = 0; for (let i = 0; i < n; i++) { s += i; } return s;
    //   -> return (n <= 0) ? 0 : ((n * (n - 1)) / 2);
    // and with `s = (s + i) | 0` in the body, the same sum ToInt32'd.
    for (const auto* list : p.lists()) {
        for (size_t i = 0; i + 2 < list->size(); ++i) {
            auto* sDecl = exactAs<const VarDecl*>((*list)[i].get());
            auto* loop = exactAs<const ForStmt*>((*list)[i + 1].get());
            auto* ret = exactAs<const ReturnStmt*>((*list)[i + 2].get());
            if (!sDecl || !loop || !ret || sDecl->isConst || sDecl->isVar || sDecl->pattern) continue;
            auto zero = literalNumber(sDecl->init.get());
            if (!zero || *zero != 0) continue;
            const std::string& s = sDecl->name;
            if (!sameIdent(ret->value.get(), s) || loop->init.size() != 1) continue;
            auto* iDecl = exactAs<const VarDecl*>(loop->init[0].get());
            if (!iDecl || iDecl->isConst || iDecl->isVar || iDecl->pattern) continue;
            auto iz = literalNumber(iDecl->init.get());
            if (!iz || *iz != 0) continue;
            const std::string& iv = iDecl->name;
            auto* cond = exactAs<const Binary*>(loop->condition.get());
            if (!cond || cond->op != BinaryOp::Less || !sameIdent(cond->lhs.get(), iv)) continue;
            const Ident* n = asIdent(cond->rhs.get());
            if (!n || n->name == s || n->name == iv || s == iv) continue;
            if (!incrementsByOne(loop->update.get(), iv) || loop->body.size() != 1) continue;
            auto* step = exactAs<const ExprStmt*>(loop->body[0].get());
            auto* asg = step ? exactAs<const Binary*>(step->expr.get()) : nullptr;
            if (!asg || !sameIdent(asg->lhs.get(), s)) continue;
            bool wraps = false;
            if (asg->op == BinaryOp::PlusAssign) {
                if (!sameIdent(asg->rhs.get(), iv)) continue;
            } else if (asg->op == BinaryOp::Assign) {
                const Expr* rhs = asg->rhs.get();
                if (auto* orz = exactAs<const Binary*>(rhs); orz && orz->op == BinaryOp::BitOr) {
                    auto z = literalNumber(orz->rhs.get());
                    if (!z || *z != 0) continue;
                    wraps = true;
                    rhs = orz->lhs.get();
                }
                auto* add = exactAs<const Binary*>(rhs);
                if (!add || add->op != BinaryOp::Add || !sameIdent(add->lhs.get(), s) ||
                    !sameIdent(add->rhs.get(), iv)) {
                    continue;
                }
            } else {
                continue;
            }
            // s and i are each declared once in the whole program and nothing
            // outside these three statements mentions them: the rewrite
            // removes both bindings.
            if (p.declarationCount(s) != 1 || p.declarationCount(iv) != 1) continue;
            auto first = p.statement(*sDecl);
            auto last = p.statement(*ret);
            if (!first || !last) continue;
            Extent ext{first->begin, last->end};
            bool mentionedOutside = false;
            for (const Ident* id : p.idents()) {
                if (id->name != s && id->name != iv) continue;
                Extent ie = p.inner(*id);
                if (ie.begin < ext.begin || ie.end > ext.end) mentionedOutside = true;
            }
            if (mentionedOutside) continue;
            const std::string& nn = n->name;
            std::string sum = "((" + nn + " * (" + nn + " - 1)) / 2)";
            std::string after = "return (" + nn + " <= 0) ? 0 : " + (wraps ? "(" + sum + " | 0)" : sum) + ";";
            out.push_back(makeEdit(p, ext, after, *ret, fallbackFn, "closed-form-series",
                                   "Reduced O(n) arithmetic accumulation loop to O(1) closed-form Gauss formula"));
        }
    }
}

}  // namespace

void collectLoopEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out) {
    collectDeforestation(p, fallbackFn, out);
    collectClosedFormSeries(p, fallbackFn, out);
}

}  // namespace bronze::check::jsrw
