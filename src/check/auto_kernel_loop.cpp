#include "check/auto_kernel_loop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "ast/ast.h"
#include "ast/clone.h"
#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"

namespace bronze::check::autokernel {
namespace {

using namespace bronze::ast;

// Bounds on an affine form's numbers, so that every sum and product the solver
// is later asked about fits a 64-bit bitvector with room to spare.
constexpr int64_t kMaxCoeff = int64_t(1) << 24;
constexpr int64_t kMaxConst = int64_t(1) << 40;

bool mulChecked(int64_t a, int64_t b, int64_t limit, int64_t& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    int64_t aa = a < 0 ? -a : a;
    int64_t bb = b < 0 ? -b : b;
    if (aa > limit / bb) return false;
    out = a * b;
    return true;
}

// dst += scale * src. False when a number leaves its bound.
bool addScaled(Affine& dst, const Affine& src, int64_t scale) {
    int64_t c = 0;
    if (!mulChecked(src.constant, scale, kMaxConst, c)) return false;
    dst.constant += c;
    if (dst.constant > kMaxConst || dst.constant < -kMaxConst) return false;
    for (const auto& [atom, k] : src.terms) {
        int64_t p = 0;
        if (!mulChecked(k, scale, kMaxCoeff, p)) return false;
        int64_t v = dst.coeff(atom) + p;
        if (v > kMaxCoeff || v < -kMaxCoeff) return false;
        if (v == 0) {
            dst.terms.erase(atom);
        } else {
            dst.terms[atom] = v;
        }
    }
    return true;
}

std::string trimmed(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

const std::set<std::string>& pureMathFunctions() {
    static const std::set<std::string> fns = {
        "abs",  "acos",  "acosh", "asin", "asinh", "atan",  "atan2", "atanh", "cbrt",
        "ceil", "clz32", "cos",   "cosh", "exp",   "expm1", "floor", "fround", "hypot",
        "imul", "log",   "log10", "log1p", "log2", "max",   "min",   "pow",   "round",
        "sign", "sin",   "sinh",  "sqrt", "tan",   "tanh",  "trunc"};
    return fns;
}

struct LocalInfo {
    const VarDecl* decl = nullptr;
    bool reassigned = false;
};

struct RawAccess {
    std::string array;
    const Expr* index = nullptr;
    bool isWrite = false;
    bool compound = false;
};

// One pass over the loop body. It rejects every construct a kernel cannot hold
// or that has an effect beyond the elements it records, and records the
// element accesses, the body's own declarations and the names it reads.
class BodyWalker {
public:
    BodyWalker(const std::string& src, std::string iv, const std::set<std::string>& params)
        : src_(src), iv_(std::move(iv)), params_(params) {}

    std::string kind, reason, rArray, rIndex;
    std::unordered_map<std::string, LocalInfo> locals;
    std::vector<RawAccess> accesses;
    uint32_t maxEnd = 0;

    bool ok() const { return reason.empty(); }

    std::string text(const Node& n) const {
        if (n.span.end <= n.span.begin || n.span.end > src_.size()) return "";
        return trimmed(std::string_view(src_).substr(n.span.begin, n.span.end - n.span.begin));
    }

    void fail(std::string k, std::string why, std::string arr = "", std::string idx = "") {
        if (!reason.empty()) return;
        kind = std::move(k);
        reason = std::move(why);
        rArray = std::move(arr);
        rIndex = std::move(idx);
    }

    void stmts(const std::vector<StmtPtr>& list) {
        for (const auto& s : list) {
            if (!ok()) return;
            if (s) stmt(*s);
        }
    }

    // Names read that are not the body's own, checked once the whole body
    // (and so every declaration in it) has been seen.
    void finish() {
        if (!ok()) return;
        for (const auto& name : pendingWrites_) {
            auto it = locals.find(name);
            if (it == locals.end()) {
                fail("SIDE_EFFECT", "Loop body writes `" + name +
                                        "`, which is bound outside the loop");
                return;
            }
            it->second.reassigned = true;
        }
        if (usesMath_ && (params_.count("Math") || locals.count("Math") || iv_ == "Math")) {
            fail("UNSUPPORTED", "`Math` is shadowed, so `Math.*` is not the pure builtin");
            return;
        }
        for (const auto& name : reads_) {
            if (name == iv_ || params_.count(name) || locals.count(name)) continue;
            if (name == "NaN" || name == "Infinity") continue;
            fail("UNSUPPORTED", "Loop body reads `" + name +
                                    "`, which is bound outside the loop and is not a parameter "
                                    "of the kernel");
            return;
        }
    }

private:
    const std::string& src_;
    std::string iv_;
    const std::set<std::string>& params_;
    std::vector<std::string> reads_;
    std::vector<std::string> pendingWrites_;
    bool usesMath_ = false;
    int loopDepth_ = 0;

    void touch(const Node& n) { maxEnd = std::max(maxEnd, n.span.end); }

    void declare(const VarDecl& d) {
        touch(d);
        if (d.isVar) {
            fail("SIDE_EFFECT",
                 "`var " + d.name + "` in the loop body is function-scoped and carries values "
                 "between iterations");
            return;
        }
        if (d.pattern || d.name.empty()) {
            fail("UNSUPPORTED", "Destructuring declaration in the loop body");
            return;
        }
        if (d.name == iv_ || params_.count(d.name) || d.name == "Math") {
            fail("UNSUPPORTED", "Declaration of `" + d.name + "` in the loop body shadows " +
                                    (d.name == iv_ ? "the induction variable" : "a parameter"));
            return;
        }
        if (locals.count(d.name)) {
            fail("UNSUPPORTED", "`" + d.name + "` is declared more than once in the loop body");
            return;
        }
        locals[d.name].decl = &d;
        if (d.init) expr(*d.init);
    }

    void stmt(const Stmt& s) {
        touch(s);
        if (auto* d = exactAs<const VarDecl*>(&s)) return declare(*d);
        if (auto* e = exactAs<const ExprStmt*>(&s)) {
            if (e->expr) expr(*e->expr);
            return;
        }
        if (auto* b = exactAs<const BlockStmt*>(&s)) return stmts(b->stmts);
        if (auto* i = exactAs<const IfStmt*>(&s)) {
            expr(*i->condition);
            stmts(i->thenBody);
            stmts(i->elseBody);
            return;
        }
        if (auto* f = exactAs<const ForStmt*>(&s)) {
            ++loopDepth_;
            stmts(f->init);
            if (f->condition) expr(*f->condition);
            if (f->update) expr(*f->update);
            stmts(f->body);
            --loopDepth_;
            return;
        }
        if (auto* w = exactAs<const WhileStmt*>(&s)) {
            ++loopDepth_;
            expr(*w->condition);
            stmts(w->body);
            --loopDepth_;
            return;
        }
        if (auto* w = exactAs<const DoWhileStmt*>(&s)) {
            ++loopDepth_;
            stmts(w->body);
            expr(*w->condition);
            --loopDepth_;
            return;
        }
        if (auto* b = exactAs<const BreakStmt*>(&s)) {
            if (loopDepth_ == 0 || !b->label.empty()) {
                fail("SIDE_EFFECT", "`break` leaves the loop being parallelized");
            }
            return;
        }
        if (auto* c = exactAs<const ContinueStmt*>(&s)) {
            if (loopDepth_ == 0 || !c->label.empty()) {
                fail("SIDE_EFFECT", "`continue` in the loop body (the kernel has no next iteration)");
            }
            return;
        }
        if (exactAs<const ReturnStmt*>(&s)) {
            return fail("SIDE_EFFECT", "`return` inside the loop body");
        }
        if (exactAs<const ThrowStmt*>(&s)) {
            return fail("SIDE_EFFECT", "`throw` inside the loop body");
        }
        if (exactAs<const TryStmt*>(&s)) return fail("UNSUPPORTED", "`try` inside the loop body");
        if (exactAs<const SwitchStmt*>(&s)) {
            return fail("UNSUPPORTED", "`switch` inside the loop body");
        }
        if (exactAs<const FunctionDecl*>(&s) || exactAs<const ClassDecl*>(&s)) {
            return fail("SIDE_EFFECT", "Function or class declaration inside the loop body");
        }
        fail("UNSUPPORTED", "Unsupported statement inside the loop body");
    }

    // `lhs` is written (and read too when `compound`).
    void target(const Expr& lhs, bool compound) {
        touch(lhs);
        if (auto* id = exactAs<const Ident*>(&lhs)) {
            if (id->name == iv_) {
                return fail("SIDE_EFFECT", "Loop body assigns the induction variable `" + iv_ + "`");
            }
            auto it = locals.find(id->name);
            if (it != locals.end()) {
                it->second.reassigned = true;
                if (compound) reads_.push_back(id->name);
                return;
            }
            if (params_.count(id->name)) {
                return fail("SIDE_EFFECT", "Loop body writes the parameter `" + id->name +
                                               "`, a value shared by every iteration");
            }
            // Not declared YET is not the same as not the body's: a later
            // declaration makes this a TDZ error in both programs, so
            // finish() decides once every declaration has been seen.
            pendingWrites_.push_back(id->name);
            return;
        }
        if (auto* ix = exactAs<const IndexAccess*>(&lhs)) {
            auto* base = exactAs<const Ident*>(ix->object.get());
            if (ix->optional || !base) {
                return fail("SIDE_EFFECT", "Element store into `" + text(*ix->object) +
                                               "`, which is not a parameter");
            }
            if (!params_.count(base->name) || base->name == iv_) {
                return fail("SIDE_EFFECT", "Element store into `" + base->name +
                                               "`, which is not a parameter");
            }
            accesses.push_back({base->name, ix->index.get(), true, compound});
            expr(*ix->index);
            return;
        }
        if (exactAs<const MemberAccess*>(&lhs)) {
            return fail("SIDE_EFFECT", "Property write `" + text(lhs) + "` in the loop body");
        }
        fail("SIDE_EFFECT", "Assignment to `" + text(lhs) + "` in the loop body");
    }

    void expr(const Expr& e) {
        if (!ok()) return;
        touch(e);
        if (exactAs<const NumberLit*>(&e) || exactAs<const StringLit*>(&e) ||
            exactAs<const BoolLit*>(&e) || exactAs<const NullLit*>(&e) ||
            exactAs<const UndefinedLit*>(&e)) {
            return;
        }
        if (auto* t = exactAs<const TemplateLit*>(&e)) {
            for (const auto& x : t->exprs) expr(*x);
            return;
        }
        if (auto* id = exactAs<const Ident*>(&e)) {
            reads_.push_back(id->name);
            return;
        }
        if (auto* u = exactAs<const Unary*>(&e)) {
            switch (u->op) {
                case UnaryOp::Delete:
                    return fail("SIDE_EFFECT", "`delete` in the loop body");
                case UnaryOp::PreInc:
                case UnaryOp::PreDec:
                case UnaryOp::PostInc:
                case UnaryOp::PostDec:
                    return target(*u->operand, /*compound=*/true);
                default:
                    return expr(*u->operand);
            }
        }
        if (auto* b = exactAs<const Binary*>(&e)) {
            if (isAssignOp(b->op)) {
                target(*b->lhs, b->op != BinaryOp::Assign);
                if (ok()) expr(*b->rhs);
                return;
            }
            if (b->op == BinaryOp::In || b->op == BinaryOp::InstanceOf) {
                return fail("UNSUPPORTED", "`in` / `instanceof` in the loop body");
            }
            expr(*b->lhs);
            expr(*b->rhs);
            return;
        }
        if (auto* t = exactAs<const Ternary*>(&e)) {
            expr(*t->condition);
            expr(*t->thenExpr);
            expr(*t->elseExpr);
            return;
        }
        if (auto* m = exactAs<const MemberAccess*>(&e)) {
            auto* base = exactAs<const Ident*>(m->object.get());
            if (!m->optional && !m->isPrivate && base) {
                if (base->name == "Math") {
                    usesMath_ = true;
                    return;
                }
                if (m->property == "length" && params_.count(base->name)) return;
            }
            return fail("UNSUPPORTED", "Property read `" + text(e) + "` in the loop body");
        }
        if (auto* ix = exactAs<const IndexAccess*>(&e)) {
            auto* base = exactAs<const Ident*>(ix->object.get());
            if (ix->optional || !base || !params_.count(base->name)) {
                return fail("UNSUPPORTED", "Element read from `" + text(*ix->object) +
                                               "`, which is not a parameter");
            }
            accesses.push_back({base->name, ix->index.get(), false, false});
            expr(*ix->index);
            return;
        }
        if (auto* c = exactAs<const Call*>(&e)) {
            auto* m = exactAs<const MemberAccess*>(c->callee.get());
            auto* base = m ? exactAs<const Ident*>(m->object.get()) : nullptr;
            if (!c->optional && m && !m->optional && base && base->name == "Math" &&
                pureMathFunctions().count(m->property)) {
                usesMath_ = true;
                touch(*c->callee);
                for (const auto& a : c->args) {
                    if (exactAs<const SpreadElement*>(a.get())) {
                        return fail("UNSUPPORTED", "Spread argument in the loop body");
                    }
                    expr(*a);
                }
                return;
            }
            return fail("SIDE_EFFECT", "Loop body calls `" + text(*c->callee) +
                                           "` (only pure `Math.*` calls are allowed)");
        }
        if (exactAs<const NewExpr*>(&e)) {
            return fail("SIDE_EFFECT", "Heap allocation (`new`) in the loop body");
        }
        if (exactAs<const ArrayLit*>(&e) || exactAs<const ObjectLit*>(&e) ||
            exactAs<const RegExpLit*>(&e)) {
            return fail("SIDE_EFFECT", "Heap allocation (literal) in the loop body");
        }
        if (exactAs<const FunctionExpr*>(&e) || exactAs<const ClassExpr*>(&e)) {
            return fail("SIDE_EFFECT", "Closure created in the loop body");
        }
        if (exactAs<const TaggedTemplate*>(&e)) {
            return fail("SIDE_EFFECT", "Tagged template call in the loop body");
        }
        if (exactAs<const YieldExpr*>(&e)) {
            return fail("SIDE_EFFECT", "`yield` / `await` in the loop body");
        }
        if (exactAs<const DestructuringAssign*>(&e)) {
            return fail("UNSUPPORTED", "Destructuring assignment in the loop body");
        }
        fail("UNSUPPORTED", "Unsupported expression `" + text(e) + "` in the loop body");
    }
};

// Affine forms over the induction variable, parameters (and their `.length`),
// constants, and body locals bound once to an affine expression.
class AffineBuilder {
public:
    AffineBuilder(std::string iv, const std::set<std::string>& params,
                  const std::unordered_map<std::string, LocalInfo>* locals, bool allowIv)
        : iv_(std::move(iv)), params_(params), locals_(locals), allowIv_(allowIv) {}

    bool build(const Expr& e, Affine& out, std::string& why, int depth = 0) const {
        out = {};
        if (depth > 32) {
            why = "index definition nests too deeply";
            return false;
        }
        if (auto* n = exactAs<const NumberLit*>(&e)) {
            double v = n->value;
            if (std::floor(v) != v || std::fabs(v) > static_cast<double>(kMaxConst)) {
                why = "non-integer or huge constant";
                return false;
            }
            out.constant = static_cast<int64_t>(v);
            return true;
        }
        if (auto* id = exactAs<const Ident*>(&e)) {
            if (id->name == iv_) {
                if (!allowIv_) {
                    why = "refers to the induction variable";
                    return false;
                }
                out.terms[kIvAtom] = 1;
                return true;
            }
            if (locals_) {
                auto it = locals_->find(id->name);
                if (it != locals_->end()) {
                    const LocalInfo& li = it->second;
                    if (li.reassigned || !li.decl || !li.decl->init) {
                        why = "`" + id->name + "` is assigned in the loop body";
                        return false;
                    }
                    return build(*li.decl->init, out, why, depth + 1);
                }
            }
            if (params_.count(id->name)) {
                out.terms[id->name] = 1;
                return true;
            }
            why = "`" + id->name + "` is not a parameter, a constant or the induction variable";
            return false;
        }
        if (auto* m = exactAs<const MemberAccess*>(&e)) {
            auto* base = exactAs<const Ident*>(m->object.get());
            if (base && !m->optional && m->property == "length" && params_.count(base->name)) {
                out.terms[base->name + ".length"] = 1;
                return true;
            }
            why = "property read";
            return false;
        }
        if (auto* u = exactAs<const Unary*>(&e)) {
            if (u->op != UnaryOp::Negate && u->op != UnaryOp::Posate) {
                why = std::string("operator ") + unaryOpName(u->op);
                return false;
            }
            Affine inner;
            if (!build(*u->operand, inner, why, depth + 1)) return false;
            if (!addScaled(out, inner, u->op == UnaryOp::Negate ? -1 : 1)) {
                why = "coefficient too large";
                return false;
            }
            return true;
        }
        if (auto* b = exactAs<const Binary*>(&e)) {
            if (b->op != BinaryOp::Add && b->op != BinaryOp::Sub && b->op != BinaryOp::Mul) {
                why = std::string("operator ") + binaryOpName(b->op);
                return false;
            }
            Affine l, r;
            if (!build(*b->lhs, l, why, depth + 1)) return false;
            if (!build(*b->rhs, r, why, depth + 1)) return false;
            bool fits = true;
            if (b->op == BinaryOp::Mul) {
                if (!l.isConstant() && !r.isConstant()) {
                    why = "product of two non-constant terms";
                    return false;
                }
                const Affine& var = l.isConstant() ? r : l;
                int64_t k = l.isConstant() ? l.constant : r.constant;
                fits = addScaled(out, var, k);
            } else {
                fits = addScaled(out, l, 1) && addScaled(out, r, b->op == BinaryOp::Sub ? -1 : 1);
            }
            if (!fits) {
                why = "coefficient too large";
                return false;
            }
            return true;
        }
        why = "not an affine expression";
        return false;
    }

private:
    std::string iv_;
    const std::set<std::string>& params_;
    const std::unordered_map<std::string, LocalInfo>* locals_;
    bool allowIv_;
};

struct Header {
    std::string iv;
    const Expr* start = nullptr;
    const Expr* bound = nullptr;
    bool inclusive = false;
};

bool isIdent(const Expr* e, const std::string& name) {
    auto* id = exactAs<const Ident*>(e);
    return id && id->name == name;
}

bool isOne(const Expr* e) {
    auto* n = exactAs<const NumberLit*>(e);
    return n && n->value == 1.0;
}

// `for (let i = START; i < BOUND; <i += 1 in any spelling>)`.
bool canonicalHeader(const ForStmt& f, Header& h) {
    if (f.init.size() != 1 || !f.condition || !f.update) return false;
    auto* d = exactAs<const VarDecl*>(f.init[0].get());
    if (!d || d->isConst || d->pattern || d->name.empty() || !d->init) return false;
    h.iv = d->name;
    h.start = d->init.get();

    auto* c = exactAs<const Binary*>(f.condition.get());
    if (!c) return false;
    if ((c->op == BinaryOp::Less || c->op == BinaryOp::LessEqual) && isIdent(c->lhs.get(), h.iv)) {
        h.bound = c->rhs.get();
        h.inclusive = c->op == BinaryOp::LessEqual;
    } else if ((c->op == BinaryOp::Greater || c->op == BinaryOp::GreaterEqual) &&
               isIdent(c->rhs.get(), h.iv)) {
        h.bound = c->lhs.get();
        h.inclusive = c->op == BinaryOp::GreaterEqual;
    } else {
        return false;
    }

    const Expr* u = f.update.get();
    if (auto* un = exactAs<const Unary*>(u)) {
        return (un->op == UnaryOp::PostInc || un->op == UnaryOp::PreInc) &&
               isIdent(un->operand.get(), h.iv);
    }
    if (auto* b = exactAs<const Binary*>(u)) {
        if (!isIdent(b->lhs.get(), h.iv)) return false;
        if (b->op == BinaryOp::PlusAssign) return isOne(b->rhs.get());
        if (b->op == BinaryOp::Assign) {
            auto* sum = exactAs<const Binary*>(b->rhs.get());
            return sum && sum->op == BinaryOp::Add &&
                   ((isIdent(sum->lhs.get(), h.iv) && isOne(sum->rhs.get())) ||
                    (isOne(sum->lhs.get()) && isIdent(sum->rhs.get(), h.iv)));
        }
    }
    return false;
}

// The first `for` with a canonical header, outermost first, in source order.
const ForStmt* findLoop(const std::vector<StmtPtr>& list, Header& h);

const ForStmt* findLoopIn(const Stmt& s, Header& h) {
    if (auto* f = exactAs<const ForStmt*>(&s)) {
        if (canonicalHeader(*f, h)) return f;
        return findLoop(f->body, h);
    }
    if (auto* b = exactAs<const BlockStmt*>(&s)) return findLoop(b->stmts, h);
    if (auto* i = exactAs<const IfStmt*>(&s)) {
        if (auto* r = findLoop(i->thenBody, h)) return r;
        return findLoop(i->elseBody, h);
    }
    if (auto* w = exactAs<const WhileStmt*>(&s)) return findLoop(w->body, h);
    if (auto* w = exactAs<const DoWhileStmt*>(&s)) return findLoop(w->body, h);
    if (auto* l = exactAs<const LabeledStmt*>(&s)) return l->body ? findLoopIn(*l->body, h) : nullptr;
    if (auto* t = exactAs<const TryStmt*>(&s)) {
        if (auto* r = findLoop(t->body, h)) return r;
        if (auto* r = findLoop(t->catchBody, h)) return r;
        return findLoop(t->finallyBody, h);
    }
    return nullptr;
}

const ForStmt* findLoop(const std::vector<StmtPtr>& list, Header& h) {
    for (const auto& s : list) {
        if (!s) continue;
        if (auto* r = findLoopIn(*s, h)) return r;
    }
    return nullptr;
}

bool opens(TokenKind k) {
    return k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace;
}
bool closes(TokenKind k) {
    return k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace;
}

// The index of the token closing the group `toks[open]` opens, or npos.
size_t matchGroup(const std::vector<Token>& toks, size_t open) {
    int depth = 0;
    for (size_t k = open; k < toks.size(); ++k) {
        if (opens(toks[k].kind)) ++depth;
        if (closes(toks[k].kind)) {
            if (--depth == 0) return k;
        }
    }
    return std::string::npos;
}

// The loop body's source text: between its braces, or its one statement.
bool bodyRegion(const std::string& src, const std::vector<Token>& toks, const ForStmt& loop,
                uint32_t maxEnd, std::string& out) {
    size_t k = 0;
    while (k < toks.size() &&
           !(toks[k].kind == TokenKind::KwFor && toks[k].span.begin == loop.span.begin)) {
        ++k;
    }
    if (k + 1 >= toks.size() || toks[k + 1].kind != TokenKind::LParen) return false;
    size_t close = matchGroup(toks, k + 1);
    if (close == std::string::npos || close + 1 >= toks.size()) return false;
    size_t s = close + 1;
    if (toks[s].kind == TokenKind::LBrace) {
        size_t e = matchGroup(toks, s);
        if (e == std::string::npos) return false;
        uint32_t b = toks[s].span.end;
        out = src.substr(b, toks[e].span.begin - b);
        return true;
    }
    // One statement: every node in it ends by `maxEnd`; take the tokens that
    // begin before it, close any group they leave open (a trailing `)`, `]`
    // or `}` that no node's span reached), and its `;`.
    size_t e = s;
    while (e + 1 < toks.size() && toks[e + 1].kind != TokenKind::EndOfFile &&
           toks[e + 1].span.begin < maxEnd) {
        ++e;
    }
    int depth = 0;
    for (size_t j = s; j <= e; ++j) {
        if (opens(toks[j].kind)) ++depth;
        if (closes(toks[j].kind)) --depth;
        if (depth < 0) return false;
    }
    while (depth > 0 && e + 1 < toks.size()) {
        ++e;
        if (opens(toks[e].kind)) ++depth;
        if (closes(toks[e].kind)) --depth;
    }
    if (depth != 0) return false;
    if (e + 1 < toks.size() && toks[e + 1].kind == TokenKind::Semicolon) ++e;
    uint32_t b = toks[s].span.begin;
    out = src.substr(b, toks[e].span.end - b);
    return true;
}

// Does `text`, parsed as a block, give the same tree as `body`? This is what
// makes the printed kernel the proven loop: the text is spliced verbatim, and
// it is only spliced when it IS the body that was analysed.
bool printsBack(const std::string& text, const std::vector<StmtPtr>& body) {
    SourceSet sources;
    const auto& buf = sources.add("auto_kernel_body.js", "{\n" + text + "\n}");
    DiagnosticSink diags;
    auto toks = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return false;
    auto reparsed = Parser(std::move(toks), diags).parseModule("body", /*forceStrict=*/true);
    if (diags.hasErrors() || !reparsed) return false;

    Module expected;
    expected.name = "body";
    expected.strict = reparsed->strict;
    auto block = std::make_unique<BlockStmt>();
    for (const auto& s : body) {
        if (s) block->stmts.push_back(cloneStmt(*s));
    }
    expected.body.push_back(std::move(block));
    return dump(expected) == dump(*reparsed);
}

}  // namespace

std::string printAffine(const Affine& a, const std::string& iv) {
    std::ostringstream oss;
    bool first = true;
    auto emitTerm = [&](int64_t k, const std::string& atom) {
        int64_t mag = k < 0 ? -k : k;
        if (first) {
            if (k < 0) oss << "-";
        } else {
            oss << (k < 0 ? " - " : " + ");
        }
        if (atom.empty()) {
            oss << mag;
        } else {
            if (mag != 1) oss << mag << " * ";
            oss << (atom == kIvAtom ? iv : atom);
        }
        first = false;
    };
    if (a.coeff(kIvAtom) != 0) emitTerm(a.coeff(kIvAtom), kIvAtom);
    for (const auto& [atom, k] : a.terms) {
        if (atom != kIvAtom) emitTerm(k, atom);
    }
    if (a.constant != 0 || first) emitTerm(a.constant, "");
    return oss.str();
}

LoopAnalysis analyzeLoopSource(std::string_view source) {
    LoopAnalysis out;
    const std::string src(source);
    SourceSet sources;
    const auto& buf = sources.add("auto_kernel.js", src);
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        out.error = diags.render(buf);
        return out;
    }
    std::vector<Token> tokensCopy = tokens;
    auto mod = Parser(std::move(tokens), diags).parseModule("auto_kernel.js");
    if (diags.hasErrors() || !mod) {
        out.error = diags.render(buf);
        return out;
    }

    const FunctionDecl* fn = nullptr;
    for (const auto& s : mod->body) {
        if ((fn = exactAs<const FunctionDecl*>(s.get()))) break;
    }
    if (!fn) {
        out.error = "no function declaration in the source";
        return out;
    }
    out.functionName = fn->name;

    std::set<std::string> params;
    bool simpleParams = true;
    for (const auto& p : fn->params) {
        if (p.name.empty() || p.pattern || p.defaultValue || p.isRest) simpleParams = false;
        out.params.push_back(p.name);
        params.insert(p.name);
    }

    Header h;
    const ForStmt* loop = findLoop(fn->body, h);
    if (!loop) return out;
    out.foundLoop = true;
    out.inductionVar = h.iv;
    out.inclusive = h.inclusive;

    BodyWalker walker(src, h.iv, params);
    out.startText = walker.text(*h.start);
    out.boundText = walker.text(*h.bound);
    auto reject = [&](std::string kind, std::string why, std::string arr = "",
                      std::string idx = "") {
        out.rejectKind = std::move(kind);
        out.rejectReason = std::move(why);
        out.rejectArray = std::move(arr);
        out.rejectIndex = std::move(idx);
    };

    if (!simpleParams) {
        reject("UNSUPPORTED", "Kernel parameters must be plain names (no patterns, defaults or rest)");
        return out;
    }
    if (params.count(h.iv)) {
        reject("UNSUPPORTED", "The induction variable `" + h.iv + "` shadows a parameter");
        return out;
    }

    AffineBuilder header(h.iv, params, nullptr, /*allowIv=*/false);
    std::string why;
    if (!header.build(*h.start, out.start, why)) {
        reject("NON_AFFINE", "Loop start `" + out.startText + "` is not affine over the parameters: " + why);
        return out;
    }
    if (!header.build(*h.bound, out.bound, why)) {
        reject("NON_AFFINE", "Loop bound `" + out.boundText + "` is not affine over the parameters: " + why);
        return out;
    }

    walker.stmts(loop->body);
    walker.finish();
    if (!walker.ok()) {
        reject(walker.kind, walker.reason, walker.rArray, walker.rIndex);
        return out;
    }

    AffineBuilder body(h.iv, params, &walker.locals, /*allowIv=*/true);
    for (const auto& raw : walker.accesses) {
        ElementAccess acc;
        acc.array = raw.array;
        acc.indexText = walker.text(*raw.index);
        acc.isWrite = raw.isWrite;
        acc.compound = raw.compound;
        acc.affine = body.build(*raw.index, acc.index, acc.whyNotAffine);
        out.accesses.push_back(std::move(acc));
    }
    for (const auto& acc : out.accesses) {
        if (acc.affine) continue;
        reject("NON_AFFINE",
               std::string("Non-affine ") + (acc.isWrite ? "write" : "read") + " index `" +
                   acc.indexText + "` on `" + acc.array + "` cannot be proven race-free (" +
                   acc.whyNotAffine + ")",
               acc.array, acc.indexText);
        return out;
    }

    std::string text;
    if (bodyRegion(src, tokensCopy, *loop, walker.maxEnd, text) && printsBack(text, loop->body)) {
        out.bodyText = std::move(text);
    }
    return out;
}

}  // namespace bronze::check::autokernel
