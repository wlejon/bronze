// Async functions, at the parser's end of them: the contextual `async`
// modifier, `await` as an operator, and the one rewrite the parser still does
// to an async body.
//
// An async body is an ORDINARY function body here, exactly as a generator's
// is. It is parsed by the same statement loop with one flag set —
// `inAsyncBody_`, which is what makes the contextual identifier `await` an
// operator (ECMA-262 13.3: AwaitExpression is a UnaryExpression, and 15.8.1
// admits it in an async body and nowhere else). What comes out is a body with
// `YieldExpr{isAwait}` nodes in it, run through `ast::liftYields` so that
// every suspension stands at a statement boundary — the same precondition the
// generator state machine needs, needed here for the same reason: an await is
// a suspension, lowering re-enters the body at the statement after it, and
// nothing held in SSA survives that edge (ast/yield_lift.h).
//
// `async` itself is NEVER a keyword. Three rules keep it an ordinary
// identifier everywhere the grammar means one:
//
//   - `async` modifies only what follows it ON THE SAME LINE (15.8.1 forbids
//     a line terminator between `async` and `function`, 15.9.1 between
//     `async` and an arrow's parameters). `async\nfunction f() {}` is
//     therefore the expression statement `async;` — ASI at the newline — and
//     then an ordinary function declaration.
//   - `async(…)` is a call of a binding named `async`: a `(` after it is an
//     argument list, never a modifier position.
//   - `let async = 1` and `async` alone are the identifier, reached because
//     nothing below claims them.
//
// Async GENERATORS (`async function*`) and `for await` are refused by name:
// the async iteration protocol (27.6) is a second driver over the same
// machine, and an unbuilt construct is a hard error, never a silent reading.

#include <memory>
#include <string>
#include <utility>

#include "ast/yield_lift.h"
#include "parse/parser.h"

namespace bronze {

using namespace ast;

// `async function` / `async x =>` / `async (…) =>` under the cursor. The
// newline test is on the token AFTER `async`, because that is where 15.8.1
// and 15.9.1 put the restriction.
bool Parser::asyncModifiesFunction() const {
    return check(TokenKind::Identifier) && peek().text == "async" &&
           peek(1).kind == TokenKind::KwFunction && !peek(1).newlineBefore;
}

bool Parser::asyncModifiesArrow() const {
    if (!check(TokenKind::Identifier) || peek().text != "async") return false;
    if (peek(1).newlineBefore) return false;
    // `async x => …`: one parameter, no parentheses.
    if (peek(1).kind == TokenKind::Identifier && peek(2).kind == TokenKind::Arrow &&
        !peek(2).newlineBefore) {
        return true;
    }
    // `async (a, b) => …`: the same scan the plain arrow decision runs,
    // started one token in.
    return peek(1).kind == TokenKind::LParen && looksLikeArrowFrom(1);
}

// looksLikeArrow's scan with a starting offset: find the `)` matching the
// `(` at `offset` and ask what follows it. One copy of the loop, so the
// plain and async decisions cannot disagree about what closes a group.
bool Parser::looksLikeArrowFrom(size_t offset) const {
    if (peek(offset).kind != TokenKind::LParen) return false;
    size_t depth = 0;
    for (size_t i = offset;; ++i) {
        const TokenKind kind = peek(i).kind;
        if (kind == TokenKind::EndOfFile) return false;
        if (kind == TokenKind::LParen) ++depth;
        else if (kind == TokenKind::RParen) {
            if (--depth == 0) {
                if (peek(i + 1).kind == TokenKind::Arrow) return true;
                return peek(i + 1).kind == TokenKind::Colon &&
                       peek(i + 3).kind == TokenKind::Arrow;
            }
        }
    }
}

// The statement-boundary lift for an await-holding body. `async.` and not
// `gen.` so a reader of a lifted dump can tell which machine a temporary
// belongs to; the file qualifier keeps two files' first async functions
// apart, exactly as parseGeneratorTail's does.
bool Parser::liftAsyncBody(std::vector<StmtPtr>& body) {
    const size_t ordinal = asyncOrdinal_++;
    const std::string prefix =
        "async." +
        (fileId_ == 0 ? std::to_string(ordinal)
                      : std::to_string(fileId_) + "." + std::to_string(ordinal)) +
        ".";
    return liftYields(body, prefix, diags_);
}

// The parameter list and body every async form shares, with the cursor on
// the '('. The parameter list is NOT part of the async body: 15.8.1 forbids
// `await` in one, and the flag is raised only over the braces — the same
// split parseGeneratorTail makes for `yield`.
bool Parser::parseAsyncFnTail(ast::FunctionExpr& fn) {
    fn.isAsync = true;
    if (!expect(TokenKind::LParen, "'(' after an async function name")) return false;
    if (!parseParams(fn.params)) return false;
    if (!expect(TokenKind::RParen, "')' after async function parameters")) return false;
    if (match(TokenKind::Colon)) fn.returnType = parseTypeAnnotation();

    const bool savedAsync = inAsyncBody_;
    const bool savedGen = inGeneratorBody_;
    inAsyncBody_ = true;
    inGeneratorBody_ = fn.isGenerator;
    fn.body = parseFunctionBody(fn.strict);
    inGeneratorBody_ = savedGen;
    inAsyncBody_ = savedAsync;
    if (diags_.hasErrors()) return false;

    if (!liftAsyncBody(fn.body)) return false;
    fn.span.end = peek().span.begin;
    // After the body, because a function's own `"use strict"` is what makes
    // its parameter list subject to the rule — the same ordering every other
    // function form takes.
    return checkStrictParams(fn.params, fn.strict);
}

StmtPtr Parser::parseAsyncFunctionDecl(bool isExported, const std::string& defaultName) {
    const Token& asyncKw = advance();  // `async`
    advance();                         // `function` (the caller checked both)
    const bool isGenerator = match(TokenKind::Star);
    auto fn = std::make_unique<FunctionDecl>();
    fn->span.begin = asyncKw.span.begin;
    fn->isExported = isExported;
    fn->isAsync = true;
    fn->isGenerator = isGenerator;

    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        // `export default async function () {}` — the anonymous hoisted
        // declaration, named `default` for the reason parseFunctionDecl
        // names its twin that.
        fn->name = defaultName;
    } else {
        const Token* name = expect(TokenKind::Identifier, "function name");
        if (!name) return nullptr;
        if (!checkStrictBindingName(name->text, name->span, "function name")) return nullptr;
        fn->name = std::string(name->text);
    }

    // The same shell dance a generator declaration does, for the same reason:
    // a declaration and an expression differ in where the value lands and in
    // nothing about the body.
    GeneratorScopeGuard guard(*this);
    ast::FunctionExpr shell;
    shell.span = fn->span;
    shell.name = fn->name;
    shell.isGenerator = isGenerator;
    if (!parseAsyncFnTail(shell)) return nullptr;
    fn->params = std::move(shell.params);
    fn->returnType = std::move(shell.returnType);
    fn->body = std::move(shell.body);
    fn->strict = shell.strict;
    fn->span.end = peek().span.begin;
    return fn;
}

ExprPtr Parser::parseAsyncFunctionExpr() {
    const Token& asyncKw = advance();  // `async`
    advance();                         // `function`
    const bool isGenerator = match(TokenKind::Star);
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = asyncKw.span.begin;
    fn->isGenerator = isGenerator;
    if (check(TokenKind::Identifier)) {
        const Token& nameTok = advance();
        if (!checkStrictBindingName(nameTok.text, nameTok.span, "function name")) return nullptr;
        fn->name = std::string(nameTok.text);
    }
    GeneratorScopeGuard guard(*this);
    if (!parseAsyncFnTail(*fn)) return nullptr;
    return fn;
}

// `async x => …` and `async (a, b) => …`. The body of the arrow production is
// re-spelled here rather than shared with parseArrowFunction because the one
// thing that differs — which flag is raised over the body — sits in the
// middle of it, and threading a flag through the plain arrow would put async
// bookkeeping on the path every ordinary arrow takes.
ExprPtr Parser::parseAsyncArrow() {
    const Token& asyncKw = advance();  // `async`
    auto fn = std::make_unique<FunctionExpr>();
    fn->isArrow = true;
    fn->isAsync = true;
    fn->span.begin = asyncKw.span.begin;

    if (check(TokenKind::Identifier)) {
        Param p;
        p.name = std::string(advance().text);
        fn->params.push_back(std::move(p));
    } else {
        if (!expect(TokenKind::LParen, "'(' before arrow parameters")) return nullptr;
        if (!parseParams(fn->params)) return nullptr;
        if (!expect(TokenKind::RParen, "')' after arrow parameters")) return nullptr;
        if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
    }
    if (!expect(TokenKind::Arrow, "'=>' after arrow parameters")) return nullptr;

    GeneratorScopeGuard guard(*this);
    const bool savedAsync = inAsyncBody_;
    inAsyncBody_ = true;
    if (check(TokenKind::LBrace)) {
        fn->body = parseFunctionBody(fn->strict);
    } else {
        // A concise body: strict exactly when the surrounding code is, and
        // stored as the `return` it is — the same two rules the plain arrow
        // records, for the same reasons.
        fn->strict = strict_;
        auto value = parseAssign();
        if (!value) {
            inAsyncBody_ = savedAsync;
            return nullptr;
        }
        auto ret = std::make_unique<ReturnStmt>();
        ret->span = value->span;
        ret->value = std::move(value);
        fn->body.push_back(std::move(ret));
    }
    inAsyncBody_ = savedAsync;
    if (diags_.hasErrors()) return nullptr;
    if (!liftAsyncBody(fn->body)) return nullptr;
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `async m() { body }` with the name consumed — the async MethodDefinition
// tail (ECMA-262 15.8). `clearSuper` is the home-object rule parseMethodTail
// documents: an object literal's method must not inherit the enclosing
// class's `super`, a class's own method must keep it.
std::unique_ptr<ast::FunctionExpr> Parser::parseAsyncMethodTail(const std::string& name,
                                                                Span nameSpan,
                                                                bool clearSuper,
                                                                bool isGenerator) {
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = nameSpan.begin;
    fn->name = name;
    fn->isGenerator = isGenerator;

    const bool savedInClassMethod = inClassMethod_;
    const std::string savedClassSuper = currentClassSuper_;
    if (clearSuper) {
        inClassMethod_ = false;
        currentClassSuper_.clear();
    }
    GeneratorScopeGuard guard(*this);
    const bool ok = parseAsyncFnTail(*fn);
    if (clearSuper) {
        inClassMethod_ = savedInClassMethod;
        currentClassSuper_ = savedClassSuper;
    }
    if (!ok || diags_.hasErrors()) return nullptr;
    return fn;
}

// `await <UnaryExpression>` (ECMA-262 13.3). Called from `parseUnaryPrefix`,
// which is the precedence the grammar gives it: `await a + 1` awaits `a` and
// then adds, and `await x ** 2` is the same early error `-x ** 2` is,
// because an AwaitExpression is a UnaryExpression and `**` refuses one on
// its left. The operand is REQUIRED — the grammar has no bare `await`.
ExprPtr Parser::parseAwaitExpr() {
    const Token& kw = advance();  // `await`
    auto node = std::make_unique<YieldExpr>();
    node->span = kw.span;
    node->isAwait = true;
    node->argument = parseUnaryPrefix();
    if (!node->argument) return nullptr;
    node->span.end = node->argument->span.end;
    return node;
}

}  // namespace bronze
