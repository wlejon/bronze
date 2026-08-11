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

// ECMA-262 12.10. A missing semicolon is supplied when the token that would
// have followed it is on a later line, closes the enclosing block, or is the
// end of input — and only then. `foo bar` on one line stays the error it was.
//
// Nothing here inspects what the expression grammar already ate: the rule is
// about the *offending token*, which is the token this is looking at, so
// `const c = 1\n+ 2` gets no semicolon after `1` — parseExpr consumed the
// `+ 2` before reaching here, which is exactly what the spec describes and
// why ASI cannot be implemented in the lexer.
bool Parser::consumeSemicolon(const char* what) {
    if (match(TokenKind::Semicolon)) return true;
    if (atLineBreak() || check(TokenKind::RBrace) || check(TokenKind::EndOfFile)) return true;
    std::string msg = std::string("expected ';' after ") + what + ", got '" +
                      std::string(peek().text.empty() ? tokenKindName(peek().kind) : peek().text) + "'";
    diags_.error(peek().span, msg);
    return false;
}

std::unique_ptr<Module> Parser::parseModule(std::string name) {
    auto mod = std::make_unique<Module>();
    mod->name = std::move(name);
    while (!check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        if (!parseStatement(mod->body)) break;
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
    std::vector<StmtPtr> res;
    parseStatement(res);
    return res;
}

namespace {
// One statement appended, or a diagnosed failure. The productions that
// really do yield exactly one node all end this way.
bool one(std::vector<StmtPtr>& out, StmtPtr stmt) {
    if (!stmt) return false;
    out.push_back(std::move(stmt));
    return true;
}
}  // namespace

bool Parser::parseStatement(std::vector<StmtPtr>& out) {
    // ECMA-262 14.4: `;` on its own is the EmptyStatement, which evaluates to
    // empty and does nothing. It contributes no node — there is nothing for a
    // node to say — which is why this appends to a list instead of returning
    // one. It still CONSUMES the semicolon, so the caller's loop makes
    // progress and no input is silently dropped.
    if (match(TokenKind::Semicolon)) return true;

    if (match(TokenKind::KwExport)) {
        if (check(TokenKind::KwFunction)) return one(out, parseFunctionDecl(/*isExported=*/true));
        error("only 'export function' is supported after 'export' for now");
        return false;
    }
    if (check(TokenKind::KwImport)) {
        // `import` lexes as a keyword but has never had a production. Without
        // this it fell through to the expression parser and reported
        // "expected expression", naming nothing — the one place bronze's
        // unimplemented syntax was not diagnosed by name.
        error("unsupported construct: import declaration (bronze has no modules yet)");
        return false;
    }
    if (check(TokenKind::KwFunction)) return one(out, parseFunctionDecl(/*isExported=*/false));
    if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
        return parseVarDecl(out);
    }
    if (check(TokenKind::KwReturn)) return one(out, parseReturn());
    if (check(TokenKind::KwIf)) return one(out, parseIf());
    if (check(TokenKind::KwWhile)) return one(out, parseWhile());
    if (check(TokenKind::KwDo)) return one(out, parseDoWhile());
    if (check(TokenKind::KwFor)) return one(out, parseFor());
    if (check(TokenKind::KwBreak)) return one(out, parseBreak());
    if (check(TokenKind::KwContinue)) return one(out, parseContinue());
    if (check(TokenKind::KwSwitch)) return one(out, parseSwitch());
    if (check(TokenKind::KwClass)) return one(out, parseClass());
    if (check(TokenKind::KwTry)) return one(out, parseTry());
    if (check(TokenKind::KwThrow)) return one(out, parseThrow());
    if (check(TokenKind::LBrace)) {
        auto blockSpan = peek().span;
        auto stmts = parseBlock();
        auto blk = std::make_unique<BlockStmt>();
        blk->span = blockSpan;
        blk->stmts = std::move(stmts);
        return one(out, std::move(blk));
    }

    auto expr = parseExpr();
    if (!expr) return false;
    if (!consumeSemicolon("expression statement")) return false;
    auto stmt = std::make_unique<ExprStmt>();
    stmt->span = expr->span;
    stmt->expr = std::move(expr);
    return one(out, std::move(stmt));
}
std::string Parser::parseTypeAnnotation() {
    const Token* t = expect(TokenKind::Identifier, "type name");
    return t ? std::string(t->text) : std::string();
}

std::vector<StmtPtr> Parser::parseBlock() {
    std::vector<StmtPtr> body;
    if (!expect(TokenKind::LBrace, "'{'")) return body;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        if (!parseStatement(body)) return body;
    }
    expect(TokenKind::RBrace, "'}' to close block");
    return body;
}

// `let a = 1, b = 2, c` — a BindingList, which is one declaration keyword and
// several bindings (ECMA-262 14.3.1). Each declarator becomes its own
// `VarDecl` so that everything downstream — hoisting, capture analysis,
// inference, lowering — keeps seeing the one shape it already understands.
//
// They are appended to the enclosing statement list rather than wrapped in a
// `BlockStmt`: a block introduces a scope, and these bindings belong to the
// scope the declaration is written in. Wrapping them would make `let a = 1,
// b = 2` declare nothing visible to the next line.
//
// The declarators are evaluated left to right, and each initializer is an
// *AssignmentExpression*, so a comma ends the initializer rather than
// continuing it — the same rule that keeps `f(a, b)` a two-argument call
// (docs/0015 decision 6).
bool Parser::parseVarDecl(std::vector<StmtPtr>& out, bool isStatement) {
    const Token& kw = advance();  // const | let | var
    const bool isConst = kw.kind == TokenKind::KwConst;
    const bool isVar = kw.kind == TokenKind::KwVar;

    bool first = true;
    for (;;) {
        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
            error("unsupported construct: destructuring declaration");
            return false;
        }
        // The first declarator's span starts at the keyword, as it did when a
        // declaration was always one binding; a later one starts at its name,
        // which is the only text that is its own.
        const uint32_t declBegin = first ? kw.span.begin : peek().span.begin;
        const Token* name = expect(TokenKind::Identifier, "variable name");
        if (!name) return false;

        auto decl = std::make_unique<VarDecl>();
        decl->span.begin = declBegin;
        decl->isConst = isConst;
        decl->isVar = isVar;
        decl->name = std::string(name->text);

        if (match(TokenKind::Colon)) decl->typeAnnotation = parseTypeAnnotation();
        if (match(TokenKind::Assign)) {
            decl->init = parseAssign();
            if (!decl->init) return false;
        } else if (isConst) {
            error("'const' declaration requires an initializer");
            return false;
        }
        decl->span.end = peek().span.begin;
        out.push_back(std::move(decl));
        first = false;
        if (!match(TokenKind::Comma)) break;
    }

    // One terminator for the whole list, whichever kind this position takes.
    return isStatement ? consumeSemicolon("declaration")
                       : expect(TokenKind::Semicolon, "';' after for init") != nullptr;
}

StmtPtr Parser::parseReturn() {
    const Token& kw = advance();
    auto ret = std::make_unique<ReturnStmt>();
    ret->span = kw.span;
    // `return` is a restricted production: a line terminator after it ends the
    // statement, so `return\n  value` returns undefined and the value becomes
    // dead code. The most famous consequence of ASI, and one bronze must
    // reproduce rather than improve on.
    if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) &&
        !check(TokenKind::EndOfFile) && !atLineBreak()) {
        ret->value = parseExpr();
        if (!ret->value) return nullptr;
    }
    if (!consumeSemicolon("return")) return nullptr;
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
            // The header takes a whole BindingList: `for (let i = 0, j = n;
            // ...)` declares both in the loop's own scope.
            if (!parseVarDecl(stmt->init, /*isStatement=*/false)) return nullptr;
        } else {
            auto e = parseExpr();
            if (!e) return nullptr;
            expect(TokenKind::Semicolon, "';' after for init");
            auto es = std::make_unique<ExprStmt>();
            es->span = e->span;
            es->expr = std::move(e);
            stmt->init.push_back(std::move(es));
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
    // Restricted, like `return`: the identifier on the next line is the next
    // statement, not this one's label.
    if (check(TokenKind::Identifier) && !atLineBreak()) {
        stmt->label = std::string(advance().text);
    }
    consumeSemicolon("break");
    return stmt;
}

StmtPtr Parser::parseContinue() {
    const Token& kw = advance();
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->span = kw.span;
    if (check(TokenKind::Identifier) && !atLineBreak()) {
        stmt->label = std::string(advance().text);
    }
    consumeSemicolon("continue");
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
    // The one restricted production with no fallback reading: `return` on its
    // own line is a statement that returns undefined, but `throw` has nothing
    // to throw, so the spec makes it a syntax error rather than inserting.
    if (atLineBreak()) {
        error("a line terminator is not allowed between 'throw' and its expression");
        return nullptr;
    }
    parseExpr();
    consumeSemicolon("throw");
    return stmt;
}

}  // namespace bronze
