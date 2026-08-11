// The token cursor, the module entry point, and one method per statement
// production. Expressions are parser_expr.cpp.

#include "parse/parser.h"

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
    if (check(TokenKind::KwClass)) return parseClass();
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

    if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
        error("unsupported construct: destructuring declaration");
        return nullptr;
    }
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
            auto stmt = std::make_unique<ForOfStmt>();
            stmt->span = kw.span;
            stmt->isConst = check(TokenKind::KwConst);
            stmt->isLet = check(TokenKind::KwLet);
            stmt->isVar = check(TokenKind::KwVar);
            advance();  // const / let / var
            const Token* name = expect(TokenKind::Identifier, "loop variable name");
            if (!name) return nullptr;
            stmt->name = std::string(name->text);
            if (check(TokenKind::Colon)) {
                advance();
                parseTypeAnnotation();  // read and discarded: a hint types nothing
            }
            advance();  // `of`
            stmt->iterable = parseExpr();
            if (!stmt->iterable) return nullptr;
            if (!expect(TokenKind::RParen, "')' after the iterable")) return nullptr;
            stmt->body = parseBlockOrSingleStmt();
            return stmt;
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

}  // namespace bronze
