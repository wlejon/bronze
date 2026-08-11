#pragma once
#include <memory>
#include <vector>

#include "ast/ast.h"
#include "lex/token.h"
#include "support/diagnostics.h"

namespace bronze {

// Recursive-descent parser over the token stream. One method per grammar
// production; expression parsing is precedence-climbing. Errors are
// diagnosed and abort the current production — there is no error recovery
// yet (a parse with errors returns nullptr and the caller must check the
// sink; partial ASTs are never returned).
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticSink& diags)
        : tokens_(std::move(tokens)), diags_(diags) {}

    // Parses a whole module (a file). Consumes ALL input: trailing tokens
    // after the last declaration are a hard error (lesson pinned by broc's
    // parser, which silently dropped modules 2..N for a week).
    std::unique_ptr<ast::Module> parseModule(std::string name);

private:
    std::vector<Token> tokens_;
    DiagnosticSink& diags_;
    size_t pos_ = 0;
    // Which class a `super` in the body being parsed belongs to, and
    // whether there is one at all. A class body is the only place `super`
    // is legal, and the parent it names is known here and nowhere later
    // (docs/0012 decision 5).
    std::string currentClassSuper_;
    bool inClassMethod_ = false;

    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool match(TokenKind kind);
    const Token* expect(TokenKind kind, const char* what);
    void error(const char* message);
    // Consumes a statement's terminating semicolon, or inserts one where
    // ECMA-262 12.10 says the program means one (docs/0014). Every statement
    // terminator goes through here; the semicolons that are punctuation of a
    // production — the two in a `for` header — go through expect() instead.
    bool consumeSemicolon(const char* what);
    // Whether a line terminator precedes the next token. The restricted
    // productions (`return`, `throw`, `break`, `continue`, postfix `++`/`--`)
    // are the only readers.
    bool atLineBreak() const { return peek().newlineBefore; }

    ast::StmtPtr parseStatement();
    ast::StmtPtr parseFunctionDecl(bool isExported);
    // `isStatement` is false inside a `for` header, where the declaration is
    // followed by the header's own semicolon and ASI must not apply.
    ast::StmtPtr parseVarDecl(bool isStatement = true);
    ast::StmtPtr parseReturn();
    ast::StmtPtr parseIf();
    ast::StmtPtr parseWhile();
    ast::StmtPtr parseDoWhile();
    ast::StmtPtr parseFor();
    ast::StmtPtr parseBreak();
    ast::StmtPtr parseContinue();
    ast::StmtPtr parseSwitch();
    ast::StmtPtr parseTry();
    ast::StmtPtr parseThrow();
    ast::StmtPtr parseClass();
    ast::ExprPtr parseSuper();
    bool parseParams(std::vector<ast::Param>& out);
    std::vector<ast::StmtPtr> parseBlock();
    std::vector<ast::StmtPtr> parseBlockOrSingleStmt();
    std::string parseTypeAnnotation();
    // The characters a string literal denotes, escapes resolved (see the
    // definition: the lexer finds the literal's end, the parser decides what
    // it means).
    std::string decodeStringLiteral(std::string_view raw, Span span);
    ast::ExprPtr parseTemplateLiteral();
    bool looksLikeArrow() const;
    ast::ExprPtr parseArrowFunction();

    ast::ExprPtr parseExpr();
    ast::ExprPtr parseBinary(int minPrecedence);
    ast::ExprPtr parseUnaryPrefix();
    ast::ExprPtr parseUnaryPostfix();
    ast::ExprPtr parsePostfixOps(ast::ExprPtr expr);
    ast::ExprPtr parseNew();
    ast::ExprPtr parsePrimary();
    // Parses "expr, expr, ..." up to and including the closing ')' (the
    // caller has already consumed the opening '('). False on error.
    bool parseArgumentList(std::vector<ast::ExprPtr>& args);
    ast::ExprPtr parseObjectLit();
    ast::ExprPtr parseArrayLit();
    ast::ExprPtr parseFunctionExpr();
};

}  // namespace bronze
