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

std::vector<StmtPtr> Parser::parseBlockOrSingleStmt() {
    if (check(TokenKind::LBrace)) {
        return parseBlock();
    }
    auto stmt = parseStatement();
    std::vector<StmtPtr> res;
    if (stmt) res.push_back(std::move(stmt));
    return res;
}

StmtPtr Parser::parseStatement() {
    if (match(TokenKind::KwExport)) {
        if (check(TokenKind::KwFunction)) return parseFunctionDecl(/*isExported=*/true);
        error("only 'export function' is supported after 'export' for now");
        return nullptr;
    }
    if (check(TokenKind::KwFunction)) return parseFunctionDecl(/*isExported=*/false);
    if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) return parseVarDecl();
    if (check(TokenKind::KwReturn)) return parseReturn();
    if (check(TokenKind::KwIf)) return parseIf();
    if (check(TokenKind::KwWhile)) return parseWhile();
    if (check(TokenKind::KwDo)) return parseDoWhile();
    if (check(TokenKind::KwFor)) return parseFor();
    if (check(TokenKind::KwBreak)) return parseBreak();
    if (check(TokenKind::KwContinue)) return parseContinue();
    if (check(TokenKind::KwSwitch)) return parseSwitch();
    if (check(TokenKind::KwTry)) return parseTry();
    if (check(TokenKind::KwThrow)) return parseThrow();
    if (check(TokenKind::LBrace)) {
        auto blockSpan = peek().span;
        auto stmts = parseBlock();
        auto blk = std::make_unique<BlockStmt>();
        blk->span = blockSpan;
        blk->stmts = std::move(stmts);
        return blk;
    }

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
    const Token& kw = advance();  // const | let | var
    auto decl = std::make_unique<VarDecl>();
    decl->span.begin = kw.span.begin;
    decl->isConst = kw.kind == TokenKind::KwConst;
    decl->isVar = kw.kind == TokenKind::KwVar;

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
    stmt->thenBody = parseBlockOrSingleStmt();
    if (match(TokenKind::KwElse)) stmt->elseBody = parseBlockOrSingleStmt();
    return stmt;
}

StmtPtr Parser::parseWhile() {
    const Token& kw = advance();
    auto stmt = std::make_unique<WhileStmt>();
    stmt->span = kw.span;
    if (!expect(TokenKind::LParen, "'(' after 'while'")) return nullptr;
    stmt->condition = parseExpr();
    if (!stmt->condition) return nullptr;
    if (!expect(TokenKind::RParen, "')' after condition")) return nullptr;
    stmt->body = parseBlockOrSingleStmt();
    return stmt;
}

StmtPtr Parser::parseDoWhile() {
    const Token& kw = advance();
    auto stmt = std::make_unique<DoWhileStmt>();
    stmt->span = kw.span;
    stmt->body = parseBlockOrSingleStmt();
    if (!expect(TokenKind::KwWhile, "'while' after 'do' body")) return nullptr;
    if (!expect(TokenKind::LParen, "'(' after 'while'")) return nullptr;
    stmt->condition = parseExpr();
    if (!stmt->condition) return nullptr;
    if (!expect(TokenKind::RParen, "')' after condition")) return nullptr;
    match(TokenKind::Semicolon);
    return stmt;
}

StmtPtr Parser::parseFor() {
    const Token& kw = advance();
    if (!expect(TokenKind::LParen, "'(' after 'for'")) return nullptr;

    if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
        size_t lookahead = 2;
        if (peek(lookahead).kind == TokenKind::Colon) lookahead += 2;
        if (peek(lookahead).kind == TokenKind::KwIn) {
            advance(); advance(); advance();
            parseExpr(); expect(TokenKind::RParen, "')'"); parseBlockOrSingleStmt();
            return std::make_unique<ForInStmt>();
        }
        if (peek(lookahead).kind == TokenKind::KwOf) {
            advance(); advance(); advance();
            parseExpr(); expect(TokenKind::RParen, "')'"); parseBlockOrSingleStmt();
            return std::make_unique<ForOfStmt>();
        }
    }

    auto stmt = std::make_unique<ForStmt>();
    stmt->span = kw.span;

    if (!check(TokenKind::Semicolon)) {
        if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
            stmt->init = parseVarDecl();
        } else {
            auto e = parseExpr();
            if (e) {
                expect(TokenKind::Semicolon, "';' after for init");
                auto es = std::make_unique<ExprStmt>();
                es->span = e->span;
                es->expr = std::move(e);
                stmt->init = std::move(es);
            }
        }
    } else {
        advance();
    }

    if (!check(TokenKind::Semicolon)) {
        stmt->condition = parseExpr();
    }
    expect(TokenKind::Semicolon, "';' after for condition");

    if (!check(TokenKind::RParen)) {
        stmt->update = parseExpr();
    }
    expect(TokenKind::RParen, "')' after for header");

    stmt->body = parseBlockOrSingleStmt();
    return stmt;
}

StmtPtr Parser::parseBreak() {
    const Token& kw = advance();
    auto stmt = std::make_unique<BreakStmt>();
    stmt->span = kw.span;
    if (check(TokenKind::Identifier)) {
        stmt->label = std::string(advance().text);
    }
    expect(TokenKind::Semicolon, "';' after break");
    return stmt;
}

StmtPtr Parser::parseContinue() {
    const Token& kw = advance();
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->span = kw.span;
    if (check(TokenKind::Identifier)) {
        stmt->label = std::string(advance().text);
    }
    expect(TokenKind::Semicolon, "';' after continue");
    return stmt;
}

StmtPtr Parser::parseSwitch() {
    const Token& kw = advance();
    auto stmt = std::make_unique<SwitchStmt>();
    stmt->span = kw.span;
    if (match(TokenKind::LParen)) {
        parseExpr();
        match(TokenKind::RParen);
    }
    if (match(TokenKind::LBrace)) {
        int depth = 1;
        while (!check(TokenKind::EndOfFile) && depth > 0) {
            if (check(TokenKind::LBrace)) depth++;
            else if (check(TokenKind::RBrace)) {
                depth--;
                if (depth == 0) { advance(); break; }
            }
            advance();
        }
    }
    return stmt;
}

StmtPtr Parser::parseTry() {
    const Token& kw = advance();
    auto stmt = std::make_unique<TryStmt>();
    stmt->span = kw.span;
    if (check(TokenKind::LBrace)) parseBlock();
    if (match(TokenKind::KwCatch)) {
        if (match(TokenKind::LParen)) {
            match(TokenKind::Identifier);
            match(TokenKind::RParen);
        }
        if (check(TokenKind::LBrace)) parseBlock();
    }
    return stmt;
}

StmtPtr Parser::parseThrow() {
    const Token& kw = advance();
    auto stmt = std::make_unique<ThrowStmt>();
    stmt->span = kw.span;
    parseExpr();
    match(TokenKind::Semicolon);
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
        {TokenKind::Assign, {BinaryOp::Assign, 0}},
        {TokenKind::PlusAssign, {BinaryOp::PlusAssign, 0}},
        {TokenKind::MinusAssign, {BinaryOp::MinusAssign, 0}},
        {TokenKind::StarAssign, {BinaryOp::StarAssign, 0}},
        {TokenKind::SlashAssign, {BinaryOp::SlashAssign, 0}},
        {TokenKind::PercentAssign, {BinaryOp::PercentAssign, 0}},
        {TokenKind::QuestionQuestion, {BinaryOp::NullishCoalescing, 1}},
        {TokenKind::PipePipe, {BinaryOp::LogicalOr, 2}},
        {TokenKind::AmpAmp, {BinaryOp::LogicalAnd, 3}},
        {TokenKind::EqualEqualEqual, {BinaryOp::StrictEq, 4}},
        {TokenKind::BangEqualEqual, {BinaryOp::StrictNe, 4}},
        {TokenKind::EqualEqual, {BinaryOp::Eq, 4}},
        {TokenKind::BangEqual, {BinaryOp::Ne, 4}},
        {TokenKind::Less, {BinaryOp::Less, 5}},
        {TokenKind::Greater, {BinaryOp::Greater, 5}},
        {TokenKind::LessEqual, {BinaryOp::LessEqual, 5}},
        {TokenKind::GreaterEqual, {BinaryOp::GreaterEqual, 5}},
        {TokenKind::Plus, {BinaryOp::Add, 6}},
        {TokenKind::Minus, {BinaryOp::Sub, 6}},
        {TokenKind::Star, {BinaryOp::Mul, 7}},
        {TokenKind::Slash, {BinaryOp::Div, 7}},
        {TokenKind::Percent, {BinaryOp::Mod, 7}},
    };
    for (const auto& entry : kOps)
        if (entry.kind == kind) return &entry.info;
    return nullptr;
}
}  // namespace

ExprPtr Parser::parseExpr() {
    auto expr = parseBinary(0);
    if (!expr) return nullptr;
    if (match(TokenKind::Question)) {
        auto ternary = std::make_unique<Ternary>();
        ternary->span.begin = expr->span.begin;
        ternary->condition = std::move(expr);
        ternary->thenExpr = parseExpr();
        if (!ternary->thenExpr) return nullptr;
        if (!expect(TokenKind::Colon, "':' in ternary expression")) return nullptr;
        ternary->elseExpr = parseExpr();
        if (!ternary->elseExpr) return nullptr;
        ternary->span.end = ternary->elseExpr->span.end;
        return ternary;
    }
    return expr;
}

ExprPtr Parser::parseBinary(int minPrecedence) {
    auto lhs = parseUnaryPrefix();
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

ExprPtr Parser::parseUnaryPrefix() {
    const Token& t = peek();
    if (match(TokenKind::Bang)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Not;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::Minus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Negate;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::Plus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Posate;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::PlusPlus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreInc;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::MinusMinus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreDec;
        u->operand = std::move(sub);
        return u;
    }
    return parseUnaryPostfix();
}

ExprPtr Parser::parseUnaryPostfix() {
    auto expr = parsePrimary();
    if (!expr) return nullptr;
    for (;;) {
        if (match(TokenKind::Dot)) {
            const Token* member = expect(TokenKind::Identifier, "property name");
            if (!member) return nullptr;
            const auto* baseIdent = dynamic_cast<const ast::Ident*>(expr.get());
            if (baseIdent && baseIdent->name == "console" && member->text == "log") {
                auto ident = std::make_unique<ast::Ident>();
                ident->span = {expr->span.begin, member->span.end};
                ident->name = "console.log";
                expr = std::move(ident);
            } else {
                auto mem = std::make_unique<MemberAccess>();
                mem->span = {expr->span.begin, member->span.end};
                mem->object = std::move(expr);
                mem->property = std::string(member->text);
                expr = std::move(mem);
            }
        } else if (match(TokenKind::LBracket)) {
            auto indexExpr = parseExpr();
            if (!indexExpr) return nullptr;
            if (!expect(TokenKind::RBracket, "']' after index expression")) return nullptr;
            auto idx = std::make_unique<IndexAccess>();
            idx->span = {expr->span.begin, peek().span.begin};
            idx->object = std::move(expr);
            idx->index = std::move(indexExpr);
            expr = std::move(idx);
        } else if (check(TokenKind::LParen)) {
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
        } else if (match(TokenKind::PlusPlus)) {
            auto u = std::make_unique<Unary>();
            u->span = {expr->span.begin, peek().span.begin};
            u->op = UnaryOp::PostInc;
            u->operand = std::move(expr);
            expr = std::move(u);
        } else if (match(TokenKind::MinusMinus)) {
            auto u = std::make_unique<Unary>();
            u->span = {expr->span.begin, peek().span.begin};
            u->op = UnaryOp::PostDec;
            u->operand = std::move(expr);
            expr = std::move(u);
        } else {
            break;
        }
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
            lit->value = std::string(t.text.substr(1, t.text.size() - 2));
            return lit;
        }
        case TokenKind::KwTrue: {
            advance();
            auto lit = std::make_unique<BoolLit>();
            lit->span = t.span;
            lit->value = true;
            return lit;
        }
        case TokenKind::KwFalse: {
            advance();
            auto lit = std::make_unique<BoolLit>();
            lit->span = t.span;
            lit->value = false;
            return lit;
        }
        case TokenKind::KwNull: {
            advance();
            auto lit = std::make_unique<NullLit>();
            lit->span = t.span;
            return lit;
        }
        case TokenKind::KwUndefined: {
            advance();
            auto lit = std::make_unique<UndefinedLit>();
            lit->span = t.span;
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
        case TokenKind::LBrace:
            return parseObjectLit();
        case TokenKind::LBracket:
            return parseArrayLit();
        case TokenKind::KwFunction:
            return parseFunctionExpr();
        default:
            error("expected expression");
            return nullptr;
    }
}

ExprPtr Parser::parseObjectLit() {
    const Token& openToken = advance();  // '{'
    auto obj = std::make_unique<ObjectLit>();
    obj->span.begin = openToken.span.begin;

    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        std::string keyStr;
        if (check(TokenKind::Identifier)) {
            keyStr = std::string(advance().text);
        } else if (check(TokenKind::StringLiteral)) {
            auto sTok = advance();
            keyStr = std::string(sTok.text.substr(1, sTok.text.size() - 2));
        } else {
            error("expected identifier or string literal for property key");
            return nullptr;
        }

        if (!expect(TokenKind::Colon, "':' after property key")) return nullptr;

        auto valExpr = parseExpr();
        if (!valExpr) return nullptr;

        obj->props.push_back(ObjectProp{std::move(keyStr), std::move(valExpr)});

        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBrace, "'}' after object literal")) return nullptr;
    obj->span.end = peek().span.begin;
    return obj;
}

ExprPtr Parser::parseArrayLit() {
    const Token& openToken = advance();  // '['
    auto arr = std::make_unique<ArrayLit>();
    arr->span.begin = openToken.span.begin;

    while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        auto elemExpr = parseExpr();
        if (!elemExpr) return nullptr;
        arr->elements.push_back(std::move(elemExpr));
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBracket, "']' after array literal")) return nullptr;
    arr->span.end = peek().span.begin;
    return arr;
}

ExprPtr Parser::parseFunctionExpr() {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = kw.span.begin;

    if (check(TokenKind::Identifier)) {
        fn->name = std::string(advance().text);
    }

    if (!expect(TokenKind::LParen, "'(' after function")) return nullptr;
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

}  // namespace bronze
