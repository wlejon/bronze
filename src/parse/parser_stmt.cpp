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

// The reserved words are a contiguous run of the enum, kept alphabetical in
// token.h between the template pieces and the punctuation — so membership is
// a range test rather than a switch that a new keyword could be forgotten
// from.
bool Parser::isIdentifierName(TokenKind kind) {
    return kind == TokenKind::Identifier ||
           (kind >= TokenKind::KwBreak && kind <= TokenKind::KwWhile);
}

const Token* Parser::expectPropertyName(const char* what) {
    if (isIdentifierName(peek().kind)) return &advance();
    return expect(TokenKind::Identifier, what);
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
    // The Script's own Directive Prologue, read before the first statement so
    // that every early error below is decided in the mode the file asked for.
    if (prologueSelectsStrict()) strict_ = true;
    mod->strict = strict_;
    while (!check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        // The one position ECMA-262 16.2 allows an `import` or an `export`.
        // The flag is cleared by parseStatement itself, so every nested
        // production this reaches sees false.
        atModuleTopLevel_ = true;
        atBodyTopLevel_ = true;
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
    const bool atModuleTop = atModuleTopLevel_;
    atModuleTopLevel_ = false;
    const bool atBodyTop = atBodyTopLevel_;
    atBodyTopLevel_ = false;
    // ECMA-262 14.4: `;` on its own is the EmptyStatement, which evaluates to
    // empty and does nothing. It contributes no node — there is nothing for a
    // node to say — which is why this appends to a list instead of returning
    // one. It still CONSUMES the semicolon, so the caller's loop makes
    // progress and no input is silently dropped.
    if (match(TokenKind::Semicolon)) return true;

    // `import` and `export` are ModuleItems (ECMA-262 16.2), not statements:
    // they are legal directly in a module body and nowhere else. A nested one
    // is a syntax error rather than something the linker later has to decide
    // the meaning of, because there is no meaning to decide — a binding
    // introduced into a block would be visible to nothing outside it.
    if (check(TokenKind::KwExport) || check(TokenKind::KwImport)) {
        if (!atModuleTop) {
            error("an import or export declaration may only appear at the top level of a module");
            return false;
        }
        return check(TokenKind::KwExport) ? parseExportDecl(out) : parseImportDecl(out);
    }
    if (check(TokenKind::KwFunction)) {
        // ECMA-262 14.1 / Annex B.3.3: in STRICT code a function declaration
        // written anywhere but directly in a script or function body is
        // block-scoped — visible inside its block and nowhere else. bronze
        // hoists every declaration to the enclosing function, which is the
        // sloppy-mode Annex B reading and a WRONG answer for strict code the
        // moment the name is read outside the block. Named here rather than
        // compiled into the other mode's scoping.
        if (strict_ && !atBodyTop) {
            error("unsupported construct: a function declaration inside a block in strict code "
                  "(ECMA-262 14.1 makes it block-scoped, and bronze hoists it to the enclosing "
                  "function); write `const f = function () { ... }` instead");
            return false;
        }
        return one(out, parseFunctionDecl(/*isExported=*/false));
    }
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
    // `with (o) stmt`. Not a keyword in bronze's lexer, so without this it
    // parsed as a CALL of a variable named `with` followed by a block, and a
    // program that used it got a wrong answer rather than a diagnostic.
    // ECMA-262 14.11.1 makes it an early SyntaxError in strict code; bronze has
    // not built its object environment record in either mode, so both readings
    // are named errors and the strict one cites the rule.
    if (check(TokenKind::Identifier) && peek().text == "with" &&
        peek(1).kind == TokenKind::LParen) {
        error(strict_ ? "strict mode: the 'with' statement is not allowed (ECMA-262 14.11.1)"
                      : "unsupported construct: the 'with' statement");
        return false;
    }
    // `name:` at the head of a statement is a label and can be nothing else —
    // no expression statement begins with an identifier followed by a colon,
    // which is why one token of lookahead settles it (ECMA-262 14.13).
    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
        return one(out, parseLabeled());
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
// continuing it — the same rule that keeps `f(a, b)` a two-argument call.
bool Parser::parseVarDecl(std::vector<StmtPtr>& out, bool isStatement) {
    const Token& kw = advance();  // const | let | var
    const bool isConst = kw.kind == TokenKind::KwConst;
    const bool isVar = kw.kind == TokenKind::KwVar;

    bool first = true;
    for (;;) {
        // The first declarator's span starts at the keyword, as it did when a
        // declaration was always one binding; a later one starts at its name,
        // which is the only text that is its own.
        const uint32_t declBegin = first ? kw.span.begin : peek().span.begin;
        auto decl = std::make_unique<VarDecl>();
        decl->span.begin = declBegin;
        decl->isConst = isConst;
        decl->isVar = isVar;

        const bool isPattern = check(TokenKind::LBracket) || check(TokenKind::LBrace);
        if (isPattern) {
            decl->pattern = parsePattern();
            if (!decl->pattern) return false;
        } else {
            const Token* name = expect(TokenKind::Identifier, "variable name");
            if (!name) return false;
            if (!checkStrictBindingName(name->text, name->span, "variable")) return false;
            decl->name = std::string(name->text);
        }

        if (match(TokenKind::Colon)) decl->typeAnnotation = parseTypeAnnotation();
        if (match(TokenKind::Assign)) {
            decl->init = parseAssign();
            if (!decl->init) return false;
        } else if (isPattern) {
            // ECMA-262 14.3.1: a BindingPattern declarator always has an
            // Initializer, for every declaration keyword. There is nothing
            // for `let [a, b];` to destructure.
            error("a destructuring declaration requires an initializer");
            return false;
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
    // A `return` in a generator body is not this function's return: it ends the
    // WALK, and its value becomes the `value` of the final result object.
    // bronze's step index only counts forwards and its `done` result carries no
    // value, so both are refused by name wherever they are written — here for
    // the ones nested inside a statement, and in `parseGeneratorTail` for the
    // ones at the top level, where the message can also say which of the two
    // forms it is.
    if (inGeneratorBody_) {
        diags_.error(kw.span,
                     "unsupported construct: `return` inside a generator body (it ends the walk "
                     "and supplies the final result's value); bronze implements the "
                     "straight-line subset only: a sequence of `yield <expr>;` statements");
        return nullptr;
    }
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

// The binding target shared by `for-in` and `for-of`: one declaration
// keyword, then a name or a pattern, then the type annotation bronze reads
// and discards (a hint types nothing).
bool Parser::parseForBindingHead(ForBindingHead& head) {
    head.isConst = check(TokenKind::KwConst);
    head.isLet = check(TokenKind::KwLet);
    head.isVar = check(TokenKind::KwVar);
    advance();  // const / let / var
    if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
        head.pattern = parsePattern();
        if (!head.pattern) return false;
    } else {
        const Token* name = expect(TokenKind::Identifier, "loop variable name");
        if (!name) return false;
        if (!checkStrictBindingName(name->text, name->span, "loop variable")) return false;
        head.name = std::string(name->text);
    }
    if (check(TokenKind::Colon)) {
        advance();
        parseTypeAnnotation();
    }
    return true;
}

StmtPtr Parser::parseFor() {
    const Token& kw = advance();
    if (!expect(TokenKind::LParen, "'(' after 'for'")) return nullptr;

    if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
        // What comes after the binding TARGET decides which of the three
        // `for` statements this is, and a target can be a pattern of
        // unbounded length — `for (const [k, v] of pairs)` — so the lookahead
        // has to skip a whole group rather than a fixed token count.
        const size_t lookahead = skipBindingTarget(1);
        if (peek(lookahead).kind == TokenKind::KwIn) {
            ForBindingHead head;
            if (!parseForBindingHead(head)) return nullptr;
            auto stmt = std::make_unique<ForInStmt>();
            stmt->span = kw.span;
            stmt->isConst = head.isConst;
            stmt->isLet = head.isLet;
            stmt->isVar = head.isVar;
            stmt->name = std::move(head.name);
            stmt->pattern = std::move(head.pattern);
            if (!expect(TokenKind::KwIn, "'in' in a for-in header")) return nullptr;
            stmt->object = parseExpr();
            if (!stmt->object) return nullptr;
            if (!expect(TokenKind::RParen, "')' after the enumerated object")) return nullptr;
            stmt->body = parseBlockOrSingleStmt();
            return stmt;
        }
        if (peek(lookahead).kind == TokenKind::KwOf) {
            ForBindingHead head;
            if (!parseForBindingHead(head)) return nullptr;
            auto stmt = std::make_unique<ForOfStmt>();
            stmt->span = kw.span;
            stmt->isConst = head.isConst;
            stmt->isLet = head.isLet;
            stmt->isVar = head.isVar;
            stmt->name = std::move(head.name);
            stmt->pattern = std::move(head.pattern);
            if (!expect(TokenKind::KwOf, "'of' in a for-of header")) return nullptr;
            stmt->iterable = parseExpr();
            if (!stmt->iterable) return nullptr;
            if (!expect(TokenKind::RParen, "')' after the iterable")) return nullptr;
            stmt->body = parseBlockOrSingleStmt();
            return stmt;
        }
    } else if (check(TokenKind::Identifier) &&
               (peek(1).kind == TokenKind::KwIn || peek(1).kind == TokenKind::KwOf)) {
        // `for (k in o)` writes a binding that already exists. Legal
        // JavaScript, and deliberately not built — without this it read as a
        // three-part header whose init expression was `k in o`, and the
        // diagnostic named the missing semicolon rather than the construct.
        error("unsupported construct: a for-in / for-of head that assigns an existing "
              "binding (write `for (const x in o)`)");
        return nullptr;
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
    if (!expect(TokenKind::LParen, "'(' after 'switch'")) return nullptr;
    stmt->discriminant = parseExpr();
    if (!stmt->discriminant) return nullptr;
    if (!expect(TokenKind::RParen, "')' after the switch discriminant")) return nullptr;
    if (!expect(TokenKind::LBrace, "'{' to open the switch body")) return nullptr;

    bool sawDefault = false;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        SwitchCase clause;
        clause.span = peek().span;
        if (match(TokenKind::KwCase)) {
            // A CaseClause's Expression is an *Expression*, comma operator
            // and all: the colon ends it, so nothing here has to stop at a
            // comma the way an argument list does.
            clause.test = parseExpr();
            if (!clause.test) return nullptr;
        } else if (match(TokenKind::KwDefault)) {
            // ECMA-262 14.12.1 splits a CaseBlock at the DefaultClause, so
            // there is room in the grammar for exactly one however the clauses
            // are ordered.
            if (sawDefault) {
                error("a switch may have only one 'default' clause");
                return nullptr;
            }
            sawDefault = true;
        } else {
            error("expected 'case' or 'default' in a switch body");
            return nullptr;
        }
        if (!expect(TokenKind::Colon, "':' after a switch case label")) return nullptr;
        // A clause holds a StatementList and not a Block: it has no braces of
        // its own, so it ends where the next clause begins. That is what makes
        // fallthrough the default rather than a feature — there is nothing
        // between one clause's last statement and the next clause's first.
        while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) &&
               !check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) &&
               !diags_.hasErrors()) {
            if (!parseStatement(clause.body)) return nullptr;
        }
        clause.span.end = peek().span.begin;
        stmt->cases.push_back(std::move(clause));
    }
    if (!expect(TokenKind::RBrace, "'}' to close the switch body")) return nullptr;
    stmt->span.end = peek().span.begin;
    return stmt;
}

StmtPtr Parser::parseLabeled() {
    const Token& name = advance();
    advance();  // ':'
    auto stmt = std::make_unique<LabeledStmt>();
    stmt->span = name.span;
    stmt->label = std::string(name.text);

    // ECMA-262 14.13.1 makes a labelled lexical declaration an early error,
    // and for a reason worth restating: the label would name a jump target
    // into the middle of a binding's initialization.
    if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwClass) ||
        check(TokenKind::KwFunction)) {
        error("a label may not front a declaration");
        return nullptr;
    }
    std::vector<StmtPtr> body;
    if (!parseStatement(body)) return nullptr;
    if (body.size() != 1) {
        // The empty statement contributes no node, and a BindingList
        // contributes several; a LabelledItem is exactly one Statement.
        error("a label must front exactly one statement");
        return nullptr;
    }
    stmt->body = std::move(body[0]);
    stmt->span.end = stmt->body->span.end;
    return stmt;
}

StmtPtr Parser::parseTry() {
    const Token& kw = advance();
    auto stmt = std::make_unique<TryStmt>();
    stmt->span = kw.span;
    if (!check(TokenKind::LBrace)) {
        error("'{' to open a try block");
        return nullptr;
    }
    stmt->body = parseBlock();
    if (diags_.hasErrors()) return nullptr;

    if (match(TokenKind::KwCatch)) {
        stmt->hasCatch = true;
        // `catch { }` with no parameter is ES2019's optional catch binding,
        // and it is not a degenerate case to tolerate: it is the spelling for
        // "I do not care what was thrown", which is common enough that the
        // grammar grew a production for it.
        if (match(TokenKind::LParen)) {
            stmt->hasCatchParam = true;
            if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
                // 14.15.1's CatchParameter is a BindingIdentifier or a
                // BindingPattern, so everything binding patterns support
                // applies unchanged — including an element default and a rest
                // element, both of which a BindingPattern admits wherever it
                // appears. Only a default on the parameter ITSELF is excluded,
                // and the grammar excludes it: there is no `=` to reach after
                // the pattern closes.
                stmt->catchPattern = parsePattern();
                if (!stmt->catchPattern) return nullptr;
            } else {
                const Token* name = expect(TokenKind::Identifier, "a catch parameter name");
                if (!name) return nullptr;
                if (!checkStrictBindingName(name->text, name->span, "catch parameter")) {
                    return nullptr;
                }
                stmt->catchName = std::string(name->text);
            }
            if (!expect(TokenKind::RParen, "')' after a catch parameter")) return nullptr;
        }
        if (!check(TokenKind::LBrace)) {
            error("'{' to open a catch block");
            return nullptr;
        }
        stmt->catchBody = parseBlock();
        if (diags_.hasErrors()) return nullptr;
    }
    // `finally` is a keyword the lexer already produces, and leaving it here
    // meant the block after it was read as an expression statement — so a
    // `try/finally` was diagnosed as stray punctuation instead of as the
    // construct lowering names. A parser must consume all of what it claims.
    if (match(TokenKind::KwFinally)) {
        stmt->hasFinally = true;
        if (!check(TokenKind::LBrace)) {
            error("'{' to open a finally block");
            return nullptr;
        }
        stmt->finallyBody = parseBlock();
        if (diags_.hasErrors()) return nullptr;
    }
    if (!stmt->hasCatch && !stmt->hasFinally) {
        error("a 'try' requires a 'catch' or a 'finally'");
        return nullptr;
    }
    stmt->span.end = peek().span.begin;
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
    stmt->value = parseExpr();
    if (!stmt->value) return nullptr;
    consumeSemicolon("throw");
    stmt->span.end = stmt->value->span.end;
    return stmt;
}

}  // namespace bronze
