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

    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool match(TokenKind kind);
    const Token* expect(TokenKind kind, const char* what);
    void error(const char* message);

    ast::StmtPtr parseStatement();
    ast::StmtPtr parseFunctionDecl(bool isExported);
    ast::StmtPtr parseVarDecl();
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
    std::vector<ast::StmtPtr> parseBlock();
    std::vector<ast::StmtPtr> parseBlockOrSingleStmt();
    std::string parseTypeAnnotation();

    ast::ExprPtr parseExpr();
    ast::ExprPtr parseBinary(int minPrecedence);
    ast::ExprPtr parseUnaryPrefix();
    ast::ExprPtr parseUnaryPostfix();
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
