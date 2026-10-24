#include "check/js_rewrite_ast.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "ast/assigned.h"
#include "ast/query_walk.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"

namespace bronze::check::jsrw {

using namespace bronze::ast;

struct ParsedJs::Facts {
    struct VarDeclAt {
        const VarDecl* decl = nullptr;
        Path path;
    };
    std::unordered_map<std::string, size_t> declCount;
    std::unordered_map<std::string, VarDeclAt> varDecls;  // the first declaration of a name
    std::unordered_set<std::string> rebound;
    std::unordered_set<std::string> mentioned;
    std::unordered_set<std::string> propertyWritten;  // `n.p = v`, `delete n.p`, `n[k]++` ...
    bool hasEval = false;
    std::unordered_map<const Node*, Path> paths;
};

namespace {

constexpr size_t kNoMatch = SIZE_MAX;

// One walk over the whole program, on the traversal every ast:: query shares
// (IdentVisitor), that records where each site sits and every declaration.
class SiteWalker final : public detail::IdentVisitor {
public:
    using detail::IdentVisitor::visit;
    SiteWalker(ParsedJs::Facts& facts,
               std::vector<const Call*>& calls,
               std::vector<const Binary*>& binaries,
               std::vector<const IfStmt*>& ifs,
               std::vector<const ReturnStmt*>& returns,
               std::vector<const std::vector<StmtPtr>*>& lists,
               std::vector<const Ident*>& idents)
        : f_(facts), calls_(calls), binaries_(binaries), ifs_(ifs), returns_(returns),
          lists_(lists), idents_(idents) {}

    void walkModule(const Module& m) { walkList(m.body, FrameKind::List); }

    void visit(const Ident& i) override {
        if (i.name == "eval") f_.hasEval = true;
        idents_.push_back(&i);
        f_.paths[&i] = path_;
        IdentVisitor::visit(i);
    }
    void visit(const Call& c) override {
        calls_.push_back(&c);
        f_.paths[&c] = path_;
        IdentVisitor::visit(c);
    }
    void visit(const Binary& b) override {
        binaries_.push_back(&b);
        if (isAssignOp(b.op)) notePropertyWrite(b.lhs.get());
        IdentVisitor::visit(b);
    }
    void visit(const Unary& u) override {
        if (u.op == UnaryOp::Delete || u.op == UnaryOp::PreInc || u.op == UnaryOp::PreDec ||
            u.op == UnaryOp::PostInc || u.op == UnaryOp::PostDec) {
            notePropertyWrite(u.operand.get());
        }
        IdentVisitor::visit(u);
    }
    void visit(const DestructuringAssign& d) override {
        notePatternTargets(d.pattern.get());
        IdentVisitor::visit(d);
    }

    void visit(const FunctionDecl& fn) override {
        declare(fn.name);
        mention(fn.name);
        enterFunction(&fn.name, fn.params, fn.body);
    }
    void visit(const FunctionExpr& fn) override {
        if (!fn.name.empty()) declare(fn.name);
        enterFunction(&fn.name, fn.params, fn.body);
    }
    void visit(const ClassDecl& c) override {
        declare(c.name);
        path_.push_back({nullptr, 0, FrameKind::Function, &c.name});
        IdentVisitor::visit(c);
        path_.pop_back();
    }
    void visit(const ClassExpr& c) override {
        if (!c.name.empty()) declare(c.name);
        path_.push_back({nullptr, 0, FrameKind::Function, &c.name});
        IdentVisitor::visit(c);
        path_.pop_back();
    }

    void visit(const BlockStmt& b) override { walkList(b.stmts, FrameKind::List); }
    void visit(const VarDecl& v) override {
        if (v.pattern) {
            for (const auto& n : patternBoundNames(*v.pattern)) declare(n);
        } else {
            declare(v.name);
            f_.varDecls.try_emplace(v.name, ParsedJs::Facts::VarDeclAt{&v, path_});
        }
        IdentVisitor::visit(v);
    }
    void visit(const ReturnStmt& r) override {
        returns_.push_back(&r);
        f_.paths[&r] = path_;
        IdentVisitor::visit(r);
    }
    void visit(const IfStmt& i) override {
        ifs_.push_back(&i);
        f_.paths[&i] = path_;
        i.condition->accept(*this);
        walkList(i.thenBody, FrameKind::List);
        walkList(i.elseBody, FrameKind::List);
    }
    void visit(const WhileStmt& w) override {
        w.condition->accept(*this);
        walkList(w.body, FrameKind::List);
    }
    void visit(const DoWhileStmt& d) override {
        walkList(d.body, FrameKind::List);
        d.condition->accept(*this);
    }
    void visit(const ForStmt& f) override {
        // The head is a list of its own: a binding declared there is the
        // loop's, and no use in the condition, update or body is "later in
        // the same list", so no fact is carried from it.
        walkList(f.init, FrameKind::List);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        walkList(f.body, FrameKind::List);
    }
    void visit(const SwitchStmt& s) override {
        if (s.discriminant) s.discriminant->accept(*this);
        for (const auto& c : s.cases) {
            if (c.test) c.test->accept(*this);
            walkList(c.body, FrameKind::SwitchCase);
        }
    }
    void visit(const ForInStmt& f) override {
        declareLoopHead(f.name, f.pattern.get(), f.isConst || f.isLet || f.isVar);
        if (!f.name.empty()) mention(f.name);
        if (f.pattern) {
            for (const auto& n : patternBoundNames(*f.pattern)) mention(n);
            detail::visitPatternExprs(f.pattern.get(), *this);
        }
        if (f.object) f.object->accept(*this);
        walkList(f.body, FrameKind::List);
    }
    void visit(const ForOfStmt& f) override {
        declareLoopHead(f.name, f.pattern.get(), f.isConst || f.isLet || f.isVar);
        if (!f.name.empty()) mention(f.name);
        if (f.pattern) {
            for (const auto& n : patternBoundNames(*f.pattern)) mention(n);
            detail::visitPatternExprs(f.pattern.get(), *this);
        }
        if (f.iterable) f.iterable->accept(*this);
        walkList(f.body, FrameKind::List);
    }
    void visit(const TryStmt& t) override {
        walkList(t.body, FrameKind::List);
        if (t.hasCatchParam) {
            if (t.catchPattern) {
                for (const auto& n : patternBoundNames(*t.catchPattern)) {
                    declare(n);
                    mention(n);
                }
                detail::visitPatternExprs(t.catchPattern.get(), *this);
            } else {
                declare(t.catchName);
                mention(t.catchName);
            }
        }
        walkList(t.catchBody, FrameKind::List);
        walkList(t.finallyBody, FrameKind::List);
    }
    void visit(const ImportDecl& d) override {
        for (const auto& s : d.specifiers) {
            declare(s.local);
            mention(s.local);
        }
    }
    void visit(const Module& m) override { walkModule(m); }

private:
    ParsedJs::Facts& f_;
    std::vector<const Call*>& calls_;
    std::vector<const Binary*>& binaries_;
    std::vector<const IfStmt*>& ifs_;
    std::vector<const ReturnStmt*>& returns_;
    std::vector<const std::vector<StmtPtr>*>& lists_;
    std::vector<const Ident*>& idents_;
    Path path_;

    void declare(const std::string& name) {
        if (!name.empty()) ++f_.declCount[name];
    }
    void declareLoopHead(const std::string& name, const BindingPattern* pattern, bool declares) {
        if (!declares) return;  // an assignment head: getDeeplyReboundNames has it
        declare(name);
        if (pattern) {
            for (const auto& n : patternBoundNames(*pattern)) declare(n);
        }
    }
    void notePropertyWrite(const Expr* target) {
        const Expr* object = nullptr;
        if (auto* m = exactAs<const MemberAccess*>(target)) object = m->object.get();
        if (auto* x = exactAs<const IndexAccess*>(target)) object = x->object.get();
        if (auto* id = exactAs<const Ident*>(object)) f_.propertyWritten.insert(id->name);
    }
    void notePatternTargets(const BindingPattern* p) {
        if (!p) return;
        for (const auto& e : p->elements) {
            if (e.target) notePropertyWrite(e.target.get());
            notePatternTargets(e.pattern.get());
        }
    }
    void walkList(const std::vector<StmtPtr>& list, FrameKind kind) {
        lists_.push_back(&list);
        path_.push_back({&list, 0, kind, nullptr});
        for (size_t i = 0; i < list.size(); ++i) {
            path_.back().index = i;
            if (list[i]) list[i]->accept(*this);
        }
        path_.pop_back();
    }
    void enterFunction(const std::string* name,
                       const std::vector<Param>& params,
                       const std::vector<StmtPtr>& body) {
        path_.push_back({nullptr, 0, FrameKind::Function, name});
        for (const auto& p : params) {
            declare(p.name);
            if (p.pattern) {
                for (const auto& n : patternBoundNames(*p.pattern)) declare(n);
            }
        }
        detail::visitParamExprs(params, *this);
        walkList(body, FrameKind::List);
        path_.pop_back();
    }
};

bool isValueEnding(TokenKind k) {
    switch (k) {
        case TokenKind::Identifier:
        case TokenKind::NumberLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::TemplateWhole:
        case TokenKind::TemplateTail:
        case TokenKind::RegExpLiteral:
        case TokenKind::PrivateName:
        case TokenKind::KwFalse:
        case TokenKind::KwTrue:
        case TokenKind::KwNull:
        case TokenKind::KwThis:
        case TokenKind::KwSuper:
        case TokenKind::KwUndefined:
        case TokenKind::KwOf:
        case TokenKind::RParen:
        case TokenKind::RBracket:
        case TokenKind::RBrace:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
            return true;
        default:
            return false;
    }
}

constexpr double kExact = 9007199254740992.0;  // 2^53

std::optional<IntRange> checked(double lo, double hi) {
    if (!(lo >= -kExact && hi <= kExact) || lo > hi) return std::nullopt;
    return IntRange{lo, hi};
}

std::optional<int> shiftCount(const Expr& e) {
    auto* n = exactAs<const NumberLit*>(&e);
    if (!n || n->value != std::floor(n->value) || n->value < 0 || n->value > 4294967295.0) {
        return std::nullopt;
    }
    return static_cast<int>(static_cast<uint32_t>(n->value) & 31u);
}

}  // namespace

ParsedJs::~ParsedJs() = default;

std::unique_ptr<ParsedJs> ParsedJs::parse(const std::string& text, std::string* error) {
    std::unique_ptr<ParsedJs> p(new ParsedJs());
    p->text_ = text;
    p->sources_ = std::make_unique<SourceSet>();
    const auto& buf = p->sources_->add("input.js", text);
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (error) *error = diags.render(buf);
        return nullptr;
    }
    p->tokens_ = tokens;
    auto mod = Parser(std::move(tokens), diags).parseModule("input.js");
    if (diags.hasErrors() || !mod) {
        if (error) *error = diags.render(buf);
        return nullptr;
    }
    p->module_ = std::move(mod);

    // The end-of-file token carries no text; every index below is a real one.
    while (!p->tokens_.empty() && p->tokens_.back().kind == TokenKind::EndOfFile) p->tokens_.pop_back();
    p->match_.assign(p->tokens_.size(), kNoMatch);
    std::vector<size_t> open;
    for (size_t i = 0; i < p->tokens_.size(); ++i) {
        TokenKind k = p->tokens_[i].kind;
        if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace) {
            open.push_back(i);
        } else if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace) {
            if (!open.empty()) {
                p->match_[open.back()] = i;
                p->match_[i] = open.back();
                open.pop_back();
            }
        }
    }

    p->facts_ = std::make_unique<Facts>();
    SiteWalker walker(*p->facts_, p->calls_, p->binaries_, p->ifs_, p->returns_, p->lists_, p->idents_);
    walker.walkModule(*p->module_);
    p->facts_->mentioned = std::move(walker.names);
    p->facts_->rebound = getDeeplyReboundNames(p->module_->body);
    return p;
}

size_t ParsedJs::tokenIndexAt(uint32_t begin) const {
    auto it = std::lower_bound(tokens_.begin(), tokens_.end(), begin,
                               [](const Token& t, uint32_t off) { return t.span.begin < off; });
    return static_cast<size_t>(it - tokens_.begin());
}

size_t ParsedJs::tokenIndexEndingAt(uint32_t end) const {
    // The last token that begins before `end`: spans the parser closes at the
    // NEXT token's start (`peek().span.begin`) end in whitespace or a comment,
    // and this is the token that really ends the node.
    auto it = std::lower_bound(tokens_.begin(), tokens_.end(), end,
                               [](const Token& t, uint32_t off) { return t.span.begin < off; });
    size_t idx = static_cast<size_t>(it - tokens_.begin());
    return idx == 0 ? 0 : idx - 1;
}

const Token* ParsedJs::tokenBefore(uint32_t offset) const {
    size_t idx = tokenIndexAt(offset);
    return idx == 0 ? nullptr : &tokens_[idx - 1];
}

Extent ParsedJs::inner(const Expr& e) const {
    uint32_t b = e.span.begin;
    uint32_t en = e.span.end;
    if (!tokens_.empty() && en > b) en = std::min(en, tokens_[tokenIndexEndingAt(en)].span.end);
    auto left = [&](const Expr* c) {
        if (c) b = std::min(b, outer(*c).begin);
    };
    auto right = [&](const Expr* c) {
        if (c) en = std::max(en, outer(*c).end);
    };
    if (auto* bin = exactAs<const Binary*>(&e)) {
        left(bin->lhs.get());
        right(bin->rhs.get());
    } else if (auto* tern = exactAs<const Ternary*>(&e)) {
        left(tern->condition.get());
        right(tern->elseExpr.get());
    } else if (auto* mem = exactAs<const MemberAccess*>(&e)) {
        left(mem->object.get());
    } else if (auto* idx = exactAs<const IndexAccess*>(&e)) {
        left(idx->object.get());
    } else if (auto* call = exactAs<const Call*>(&e)) {
        left(call->callee.get());
    } else if (auto* tag = exactAs<const TaggedTemplate*>(&e)) {
        left(tag->tag.get());
    } else if (auto* un = exactAs<const Unary*>(&e)) {
        if (un->op == UnaryOp::PostInc || un->op == UnaryOp::PostDec) left(un->operand.get());
        else right(un->operand.get());
    }
    return {b, en};
}

Extent ParsedJs::outer(const Expr& e) const {
    Extent x = inner(e);
    if (!e.parenthesized || tokens_.empty()) return x;
    // Only ever asked of an operand at its parent's edge (inner() above), so
    // a matched pair directly around it is a grouping of that operand: the
    // parent's own token (its operator, `.`, `(`) sits outside the pair.
    size_t bi = tokenIndexAt(x.begin);
    size_t ei = tokenIndexEndingAt(x.end);
    while (bi > 0 && ei + 1 < tokens_.size() && tokens_[bi - 1].kind == TokenKind::LParen &&
           match_[bi - 1] == ei + 1) {
        --bi;
        ++ei;
    }
    return {tokens_[bi].span.begin, tokens_[ei].span.end};
}

std::optional<size_t> ParsedJs::bodyLastToken(size_t firstToken,
                                              const std::vector<StmtPtr>& body) const {
    if (firstToken >= tokens_.size()) return std::nullopt;
    if (tokens_[firstToken].kind == TokenKind::LBrace) {
        if (match_[firstToken] == kNoMatch) return std::nullopt;
        return match_[firstToken];
    }
    if (tokens_[firstToken].kind == TokenKind::Semicolon && body.empty()) return firstToken;
    if (body.size() != 1 || !body[0]) return std::nullopt;
    return statementLastToken(*body[0]);
}

std::optional<size_t> ParsedJs::statementLastToken(const Stmt& s) const {
    if (tokens_.empty()) return std::nullopt;
    auto withSemi = [&](size_t last) -> size_t {
        if (last + 1 < tokens_.size() && tokens_[last + 1].kind == TokenKind::Semicolon) return last + 1;
        return last;
    };
    size_t kw = tokenIndexAt(s.span.begin);
    if (kw >= tokens_.size()) return std::nullopt;
    if (auto* r = exactAs<const ReturnStmt*>(&s)) {
        return withSemi(r->value ? tokenIndexEndingAt(outer(*r->value).end) : kw);
    }
    if (auto* t = exactAs<const ThrowStmt*>(&s)) {
        return withSemi(tokenIndexEndingAt(outer(*t->value).end));
    }
    if (auto* e = exactAs<const ExprStmt*>(&s)) {
        return withSemi(tokenIndexEndingAt(outer(*e->expr).end));
    }
    if (auto* b = exactAs<const BreakStmt*>(&s)) {
        return withSemi(b->label.empty() ? kw : kw + 1);
    }
    if (auto* c = exactAs<const ContinueStmt*>(&s)) {
        return withSemi(c->label.empty() ? kw : kw + 1);
    }
    if (auto* v = exactAs<const VarDecl*>(&s)) {
        return withSemi(v->init ? tokenIndexEndingAt(outer(*v->init).end) : tokenIndexEndingAt(s.span.end));
    }
    if (exactAs<const BlockStmt*>(&s)) {
        if (tokens_[kw].kind != TokenKind::LBrace || match_[kw] == kNoMatch) return std::nullopt;
        return match_[kw];
    }
    auto parenThenBody = [&](const std::vector<StmtPtr>& body) -> std::optional<size_t> {
        size_t lp = kw + 1;
        if (lp >= tokens_.size() || tokens_[lp].kind != TokenKind::LParen || match_[lp] == kNoMatch) {
            return std::nullopt;
        }
        return bodyLastToken(match_[lp] + 1, body);
    };
    if (auto* i = exactAs<const IfStmt*>(&s)) {
        auto thenEnd = parenThenBody(i->thenBody);
        if (!thenEnd) return std::nullopt;
        size_t next = *thenEnd + 1;
        if (next < tokens_.size() && tokens_[next].kind == TokenKind::KwElse) {
            return bodyLastToken(next + 1, i->elseBody);
        }
        return thenEnd;
    }
    if (auto* w = exactAs<const WhileStmt*>(&s)) return parenThenBody(w->body);
    if (auto* f = exactAs<const ForStmt*>(&s)) return parenThenBody(f->body);
    if (auto* f = exactAs<const ForOfStmt*>(&s)) return parenThenBody(f->body);
    if (auto* f = exactAs<const ForInStmt*>(&s)) return parenThenBody(f->body);
    return std::nullopt;
}

std::optional<Extent> ParsedJs::statement(const Stmt& s) const {
    auto last = statementLastToken(s);
    if (!last) return std::nullopt;
    size_t first = tokenIndexAt(s.span.begin);
    if (first >= tokens_.size() || *last < first) return std::nullopt;
    return Extent{tokens_[first].span.begin, tokens_[*last].span.end};
}

bool ParsedJs::hasElse(const IfStmt& s) const {
    if (!s.elseBody.empty()) return true;
    size_t kw = tokenIndexAt(s.span.begin);
    size_t lp = kw + 1;
    if (lp >= tokens_.size() || match_[lp] == kNoMatch) return true;  // cannot tell: say yes
    auto thenEnd = bodyLastToken(match_[lp] + 1, s.thenBody);
    if (!thenEnd) return true;
    return *thenEnd + 1 < tokens_.size() && tokens_[*thenEnd + 1].kind == TokenKind::KwElse;
}

std::string ParsedJs::operandText(const Expr& e) const {
    std::string t = exprText(e);
    bool primary = exactAs<const Ident*>(&e) || exactAs<const NumberLit*>(&e) ||
                   exactAs<const Call*>(&e) || exactAs<const MemberAccess*>(&e) ||
                   exactAs<const IndexAccess*>(&e);
    if (auto* n = exactAs<const NumberLit*>(&e)) primary = n->value >= 0;
    return primary ? t : "(" + t + ")";
}

std::string ParsedJs::indentAt(uint32_t offset) const {
    size_t lineStart = text_.rfind('\n', offset == 0 ? 0 : offset - 1);
    lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
    size_t i = lineStart;
    while (i < text_.size() && (text_[i] == ' ' || text_[i] == '\t')) ++i;
    return text_.substr(lineStart, i - lineStart);
}

bool ParsedJs::isGlobal(const std::string& name) const {
    return !facts_->hasEval && declarationCount(name) == 0 && !facts_->rebound.count(name) &&
           !facts_->propertyWritten.count(name);
}

bool ParsedJs::isFreshName(const std::string& name) const {
    return !facts_->mentioned.count(name) && declarationCount(name) == 0;
}

size_t ParsedJs::declarationCount(const std::string& name) const {
    auto it = facts_->declCount.find(name);
    return it == facts_->declCount.end() ? 0 : it->second;
}

bool ParsedJs::isMathCall(const Call& c, std::string_view method) const {
    if (c.optional) return false;
    auto* m = exactAs<const MemberAccess*>(c.callee.get());
    if (!m || m->optional || m->isPrivate || m->property != method) return false;
    auto* base = exactAs<const Ident*>(m->object.get());
    if (!base || base->name != "Math" || !isGlobal("Math")) return false;
    for (const auto& a : c.args) {
        if (exactAs<const SpreadElement*>(a.get())) return false;
    }
    return true;
}

const Path* ParsedJs::pathOf(const Node& n) const {
    auto it = facts_->paths.find(&n);
    return it == facts_->paths.end() ? nullptr : &it->second;
}

const Expr* ParsedJs::constInit(const Ident& use) const {
    if (facts_->hasEval || declarationCount(use.name) != 1 || facts_->rebound.count(use.name)) {
        return nullptr;
    }
    auto d = facts_->varDecls.find(use.name);
    if (d == facts_->varDecls.end()) return nullptr;
    const VarDecl& v = *d->second.decl;
    if (v.isVar || v.pattern || !v.init) return nullptr;
    const Path* up = pathOf(use);
    const Path& dp = d->second.path;
    if (!up || dp.empty() || dp.back().kind != FrameKind::List) return nullptr;
    const size_t k = dp.size() - 1;
    if (up->size() <= k) return nullptr;
    for (size_t i = 0; i < k; ++i) {
        if (dp[i].list != (*up)[i].list || dp[i].index != (*up)[i].index || dp[i].kind != (*up)[i].kind) {
            return nullptr;
        }
    }
    if ((*up)[k].list != dp[k].list || (*up)[k].index <= dp[k].index) return nullptr;
    for (size_t i = k + 1; i < up->size(); ++i) {
        if ((*up)[i].kind == FrameKind::Function) return nullptr;
    }
    return v.init.get();
}

std::optional<IntRange> ParsedJs::rangeOf(const Expr& e) const { return rangeOf(e, 0); }

std::optional<IntRange> ParsedJs::rangeOf(const Expr& e, int depth) const {
    if (depth > 32) return std::nullopt;
    const IntRange int32{-2147483648.0, 2147483647.0};
    if (auto* n = exactAs<const NumberLit*>(&e)) {
        if (n->value != std::floor(n->value) || std::fabs(n->value) > kExact) return std::nullopt;
        return IntRange{n->value, n->value};
    }
    if (auto* id = exactAs<const Ident*>(&e)) {
        const Expr* init = constInit(*id);
        return init ? rangeOf(*init, depth + 1) : std::nullopt;
    }
    if (auto* t = exactAs<const Ternary*>(&e)) {
        auto a = rangeOf(*t->thenExpr, depth + 1);
        auto b = rangeOf(*t->elseExpr, depth + 1);
        if (!a || !b) return std::nullopt;
        return IntRange{std::min(a->lo, b->lo), std::max(a->hi, b->hi)};
    }
    if (auto* c = exactAs<const Call*>(&e)) {
        if (isMathCall(*c, "imul") && c->args.size() == 2) return int32;
        return std::nullopt;
    }
    if (auto* u = exactAs<const Unary*>(&e)) {
        auto r = rangeOf(*u->operand, depth + 1);
        switch (u->op) {
            case UnaryOp::Posate:
                return r;
            case UnaryOp::Negate:
                // -0 is not an integer this range can describe.
                if (!r || (r->lo <= 0 && r->hi >= 0)) return std::nullopt;
                return IntRange{-r->hi, -r->lo};
            case UnaryOp::BitNot:
                if (r && r->isInt32()) return IntRange{-r->hi - 1, -r->lo - 1};
                return int32;
            default:
                return std::nullopt;
        }
    }
    auto* b = exactAs<const Binary*>(&e);
    if (!b) return std::nullopt;
    switch (b->op) {
        case BinaryOp::BitAnd: {
            // Each side is ToInt32'd; a side that already is a non-negative
            // int32 in [0, h] bounds the result to [0, h].
            auto l = rangeOf(*b->lhs, depth + 1);
            auto r = rangeOf(*b->rhs, depth + 1);
            double hi = -1;
            if (l && l->isInt32() && l->lo >= 0) hi = l->hi;
            if (r && r->isInt32() && r->lo >= 0) hi = (hi < 0) ? r->hi : std::min(hi, r->hi);
            if (hi >= 0) return IntRange{0, hi};
            return int32;
        }
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
            return int32;
        case BinaryOp::Shr: {
            auto sh = shiftCount(*b->rhs);
            if (!sh) return int32;
            double div = std::ldexp(1.0, *sh);
            auto l = rangeOf(*b->lhs, depth + 1);
            IntRange base = (l && l->isInt32()) ? *l : int32;
            return IntRange{std::floor(base.lo / div), std::floor(base.hi / div)};
        }
        case BinaryOp::UShr: {
            auto sh = shiftCount(*b->rhs);
            auto l = rangeOf(*b->lhs, depth + 1);
            bool nonNegInt32 = l && l->isInt32() && l->lo >= 0;
            double div = std::ldexp(1.0, sh ? *sh : 0);
            if (!sh) return IntRange{0, 4294967295.0};
            if (nonNegInt32) return IntRange{std::floor(l->lo / div), std::floor(l->hi / div)};
            return IntRange{0, std::floor(4294967295.0 / div)};
        }
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul: {
            auto l = rangeOf(*b->lhs, depth + 1);
            auto r = rangeOf(*b->rhs, depth + 1);
            if (!l || !r) return std::nullopt;
            if (b->op == BinaryOp::Add) return checked(l->lo + r->lo, l->hi + r->hi);
            if (b->op == BinaryOp::Sub) return checked(l->lo - r->hi, l->hi - r->lo);
            // 0 * -k is -0: only products of non-negative ranges are described.
            if (l->lo < 0 || r->lo < 0) return std::nullopt;
            return checked(l->lo * r->lo, l->hi * r->hi);
        }
        default:
            return std::nullopt;
    }
}

std::string ParsedJs::functionNameOf(const Node& n) const {
    const Path* p = pathOf(n);
    if (!p) return "";
    for (auto it = p->rbegin(); it != p->rend(); ++it) {
        if (it->kind == FrameKind::Function && it->functionName && !it->functionName->empty()) {
            return *it->functionName;
        }
    }
    return "";
}

std::string ParsedJs::asiSafe(Extent at, std::string text) const {
    if (text.empty()) return text;
    char c = text[0];
    if (c != '(' && c != '[' && c != '`' && c != '+' && c != '-' && c != '/') return text;
    size_t first = tokenIndexAt(at.begin);
    const Token* prev = tokenBefore(at.begin);
    if (!prev || first >= tokens_.size() || !tokens_[first].newlineBefore) return text;
    return isValueEnding(prev->kind) ? ";" + text : text;
}

Edit deletionEdit(const ParsedJs& p, Extent at) {
    Edit e;
    e.at = at;
    const Token* prev = p.tokenBefore(at.begin);
    bool listContext = !prev || prev->kind == TokenKind::Semicolon || prev->kind == TokenKind::LBrace;
    if (!listContext) {
        e.text = ";";
        return e;
    }
    const std::string& t = p.text();
    size_t b = at.begin;
    while (b > 0 && (t[b - 1] == ' ' || t[b - 1] == '\t')) --b;
    size_t en = at.end;
    while (en < t.size() && (t[en] == ' ' || t[en] == '\t')) ++en;
    bool lineStart = (b == 0 || t[b - 1] == '\n');
    bool lineEnd = (en == t.size() || t[en] == '\n' || t[en] == '\r');
    if (lineStart && lineEnd) {
        if (en < t.size() && t[en] == '\r') ++en;
        if (en < t.size() && t[en] == '\n') ++en;
        e.at = {static_cast<uint32_t>(b), static_cast<uint32_t>(en)};
    }
    return e;
}

std::string applyEdits(const std::string& text,
                       std::vector<Edit> edits,
                       std::vector<JsRewritePassHit>& hits) {
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        if (a.at.begin != b.at.begin) return a.at.begin < b.at.begin;
        return a.at.end > b.at.end;
    });
    std::vector<Edit> kept;
    uint32_t lastEnd = 0;
    bool any = false;
    for (auto& e : edits) {
        if (e.at.end < e.at.begin || e.at.end > text.size()) continue;
        if (any && e.at.begin < lastEnd) continue;  // overlaps one already taken
        lastEnd = e.at.end;
        any = true;
        kept.push_back(std::move(e));
    }
    std::string out = text;
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
        out.replace(it->at.begin, it->at.end - it->at.begin, it->text);
    }
    for (auto& e : kept) hits.push_back(std::move(e.hit));
    return out;
}

std::string fnv1aHex(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

}  // namespace bronze::check::jsrw
