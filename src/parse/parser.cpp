#include "parse/parser.h"

#include <charconv>

namespace bronze {

using namespace ast;

const Token& Parser::peek(size_t ahead) const {
    const size_t idx = pos_ + ahead;
    return idx < tokens_.size() ? tokens_[idx] : tokens_.back();  // back() is EOF
}

const Token& Parser::advance() {
    const Token& t = peek();
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return t;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

const Token* Parser::expect(TokenKind kind, const char* what) {
    if (check(kind)) return &advance();
    std::string msg = std::string("expected ") + what + ", got '" +
                      std::string(peek().text.empty() ? tokenKindName(peek().kind) : peek().text) + "'";
    diags_.error(peek().span, msg);
    return nullptr;
}

void Parser::error(const char* message) { diags_.error(peek().span, message); }

std::unique_ptr<Module> Parser::parseModule(std::string name) {
    auto mod = std::make_unique<Module>();
    mod->name = std::move(name);
    while (!check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        auto stmt = parseStatement();
        if (!stmt) break;
        mod->body.push_back(std::move(stmt));
    }
    if (diags_.hasErrors()) return nullptr;
    if (!check(TokenKind::EndOfFile)) {
        error("unconsumed input after last declaration");
        return nullptr;
    }
    return mod;
}

StmtPtr Parser::parseStatement() {
    if (match(TokenKind::KwExport)) {
        if (check(TokenKind::KwFunction)) return parseFunctionDecl(/*isExported=*/true);
        error("only 'export function' is supported after 'export' for now");
        return nullptr;
    }
    if (check(TokenKind::KwFunction)) return parseFunctionDecl(/*isExported=*/false);
    if (check(TokenKind::KwConst) || check(TokenKind::KwLet)) return parseVarDecl();
    if (check(TokenKind::KwReturn)) return parseReturn();
    if (check(TokenKind::KwIf)) return parseIf();

    auto expr = parseExpr();
    if (!expr) return nullptr;
    if (!expect(TokenKind::Semicolon, "';' after expression statement")) return nullptr;
    auto stmt = std::make_unique<ExprStmt>();
    stmt->span = expr->span;
    stmt->expr = std::move(expr);
    return stmt;
}

StmtPtr Parser::parseFunctionDecl(bool isExported) {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionDecl>();
    fn->span.begin = kw.span.begin;
    fn->isExported = isExported;

    const Token* name = expect(TokenKind::Identifier, "function name");
    if (!name) return nullptr;
    fn->name = std::string(name->text);

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;
    while (!check(TokenKind::RParen)) {
        const Token* param = expect(TokenKind::Identifier, "parameter name");
        if (!param) return nullptr;
        Param p;
        p.name = std::string(param->text);
        if (match(TokenKind::Colon)) p.typeAnnotation = parseTypeAnnotation();
        fn->params.push_back(std::move(p));
        if (!match(TokenKind::Comma)) break;
    }
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseBlock();
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

std::string Parser::parseTypeAnnotation() {
    // Types are captured as raw identifier text for now; the type system is
    // its own module and will replace this with a real type expression tree.
    const Token* t = expect(TokenKind::Identifier, "type name");
    return t ? std::string(t->text) : std::string();
}

std::vector<StmtPtr> Parser::parseBlock() {
    std::vector<StmtPtr> body;
    if (!expect(TokenKind::LBrace, "'{'")) return body;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        auto stmt = parseStatement();
        if (!stmt) return body;
        body.push_back(std::move(stmt));
    }
    expect(TokenKind::RBrace, "'}' to close block");
    return body;
}

StmtPtr Parser::parseVarDecl() {
    const Token& kw = advance();  // const | let
    auto decl = std::make_unique<VarDecl>();
    decl->span.begin = kw.span.begin;
    decl->isConst = kw.kind == TokenKind::KwConst;

    const Token* name = expect(TokenKind::Identifier, "variable name");
    if (!name) return nullptr;
    decl->name = std::string(name->text);

    if (match(TokenKind::Colon)) decl->typeAnnotation = parseTypeAnnotation();
    if (match(TokenKind::Assign)) {
        decl->init = parseExpr();
        if (!decl->init) return nullptr;
    } else if (decl->isConst) {
        error("'const' declaration requires an initializer");
        return nullptr;
    }
    if (!expect(TokenKind::Semicolon, "';' after declaration")) return nullptr;
    decl->span.end = peek().span.begin;
    return decl;
}

StmtPtr Parser::parseReturn() {
    const Token& kw = advance();
    auto ret = std::make_unique<ReturnStmt>();
    ret->span = kw.span;
    if (!check(TokenKind::Semicolon)) {
        ret->value = parseExpr();
        if (!ret->value) return nullptr;
    }
    if (!expect(TokenKind::Semicolon, "';' after return")) return nullptr;
    return ret;
}

StmtPtr Parser::parseIf() {
    const Token& kw = advance();
    auto stmt = std::make_unique<IfStmt>();
    stmt->span = kw.span;
    if (!expect(TokenKind::LParen, "'(' after 'if'")) return nullptr;
    stmt->condition = parseExpr();
    if (!stmt->condition) return nullptr;
    if (!expect(TokenKind::RParen, "')' after condition")) return nullptr;
    stmt->thenBody = parseBlock();
    if (match(TokenKind::KwElse)) stmt->elseBody = parseBlock();
    return stmt;
}

// ---- Expressions ------------------------------------------------------------

namespace {
struct OpInfo {
    BinaryOp op;
    int precedence;
};
const OpInfo* binaryOpInfo(TokenKind kind) {
    static constexpr struct {
        TokenKind kind;
        OpInfo info;
    } kOps[] = {
        {TokenKind::EqualEqualEqual, {BinaryOp::StrictEq, 1}},
        {TokenKind::BangEqualEqual, {BinaryOp::StrictNe, 1}},
        {TokenKind::EqualEqual, {BinaryOp::Eq, 1}},
        {TokenKind::BangEqual, {BinaryOp::Ne, 1}},
        {TokenKind::Less, {BinaryOp::Less, 2}},
        {TokenKind::Greater, {BinaryOp::Greater, 2}},
        {TokenKind::Plus, {BinaryOp::Add, 3}},
        {TokenKind::Minus, {BinaryOp::Sub, 3}},
        {TokenKind::Star, {BinaryOp::Mul, 4}},
        {TokenKind::Slash, {BinaryOp::Div, 4}},
    };
    for (const auto& entry : kOps)
        if (entry.kind == kind) return &entry.info;
    return nullptr;
}
}  // namespace

ExprPtr Parser::parseExpr() { return parseBinary(1); }

ExprPtr Parser::parseBinary(int minPrecedence) {
    auto lhs = parseUnaryPostfix();
    if (!lhs) return nullptr;
    for (;;) {
        const OpInfo* info = binaryOpInfo(peek().kind);
        if (!info || info->precedence < minPrecedence) return lhs;
        advance();
        auto rhs = parseBinary(info->precedence + 1);
        if (!rhs) return nullptr;
        auto bin = std::make_unique<Binary>();
        bin->span = {lhs->span.begin, rhs->span.end};
        bin->op = info->op;
        bin->lhs = std::move(lhs);
        bin->rhs = std::move(rhs);
        lhs = std::move(bin);
    }
}

ExprPtr Parser::parseUnaryPostfix() {
    auto expr = parsePrimary();
    if (!expr) return nullptr;
    while (check(TokenKind::LParen)) {
        advance();
        auto call = std::make_unique<Call>();
        call->span.begin = expr->span.begin;
        call->callee = std::move(expr);
        while (!check(TokenKind::RParen)) {
            auto arg = parseExpr();
            if (!arg) return nullptr;
            call->args.push_back(std::move(arg));
            if (!match(TokenKind::Comma)) break;
        }
        if (!expect(TokenKind::RParen, "')' after arguments")) return nullptr;
        call->span.end = peek().span.begin;
        expr = std::move(call);
    }
    return expr;
}

ExprPtr Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
        case TokenKind::NumberLiteral: {
            advance();
            auto lit = std::make_unique<NumberLit>();
            lit->span = t.span;
            double value = 0;
            const auto* begin = t.text.data();
            std::from_chars(begin, begin + t.text.size(), value);
            lit->value = value;
            return lit;
        }
        case TokenKind::StringLiteral: {
            advance();
            auto lit = std::make_unique<StringLit>();
            lit->span = t.span;
            // Strip quotes; escapes stay raw until the string decoder lands.
            lit->value = std::string(t.text.substr(1, t.text.size() - 2));
            return lit;
        }
        case TokenKind::Identifier: {
            advance();
            auto ident = std::make_unique<Ident>();
            ident->span = t.span;
            ident->name = std::string(t.text);
            return ident;
        }
        case TokenKind::LParen: {
            advance();
            auto inner = parseExpr();
            if (!inner) return nullptr;
            if (!expect(TokenKind::RParen, "')'")) return nullptr;
            return inner;
        }
        default:
            error("expected expression");
            return nullptr;
    }
}

}  // namespace bronze
