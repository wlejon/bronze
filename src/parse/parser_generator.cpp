// Generators, at the parser's end of them: `yield` as an expression, the body
// that may contain one, and the one rewrite the parser still does to it.
//
// A generator body is an ORDINARY function body here. It is parsed by the same
// statement loop every other body takes, with one flag set — `inGeneratorBody_`,
// which is what makes the contextual identifier `yield` an operator (ECMA-262
// 15.5.1 admits it in an AssignmentExpression position and nowhere else). The
// body reaches the AST intact, `yield` and all, and everything downstream learns
// that generators exist: the state machine is built in src/lower.
//
// The one thing that happens here is `ast::liftYields`, which moves every
// suspension to a statement boundary. It is a rewrite of the body's SHAPE and
// not of its meaning, and it belongs on this side of the AST because it is the
// precondition lowering's re-entry needs; see ast/yield_lift.h for why.
//
// `yield*` is the same node with a flag. Delegation is a protocol rather than a
// second operator (27.5.3.7), and the whole of that protocol is built in
// src/lower/lower_yield_star.cpp; here it is one token and one operand that is
// no longer optional.

#include <memory>
#include <string>
#include <utility>

#include "ast/yield_lift.h"
#include "parse/parser.h"

namespace bronze {

using namespace ast;

// `yield`, `yield <AssignmentExpression>` and `yield* <AssignmentExpression>`
// (ECMA-262 15.5.1). Called from `parseAssign`, which is the precedence the
// grammar gives it: `yield a + 1` yields the sum, and `x = yield v` assigns what
// the resumption supplied.
ExprPtr Parser::parseYieldExpr() {
    const Token& kw = advance();  // `yield`
    auto node = std::make_unique<YieldExpr>();
    node->span = kw.span;
    // 15.5.1 writes the delegating form `yield [no LineTerminator here] *
    // AssignmentExpression`, so a line break before the star is not a
    // delegation at all — it ends a bare `yield`, and the `*` after it is
    // whatever the next statement makes of it.
    if (!atLineBreak() && check(TokenKind::Star)) {
        advance();  // `*`
        node->delegate = true;
        // The operand is REQUIRED here, unlike a plain `yield`: the grammar has
        // no production for a bare `yield*`, and a delegation with nothing to
        // delegate to has no iterator to open.
        node->argument = parseAssign();
        if (!node->argument) return nullptr;
        node->span.end = node->argument->span.end;
        return node;
    }
    // The operand is optional, and 15.5.1's restricted production ends the
    // expression at a line terminator: `yield\n x` yields undefined and `x` is
    // a statement of its own. The closers are the tokens no
    // AssignmentExpression can begin with.
    const bool bare = check(TokenKind::Semicolon) || check(TokenKind::RBrace) ||
                      check(TokenKind::RParen) || check(TokenKind::RBracket) ||
                      check(TokenKind::Comma) || check(TokenKind::Colon) ||
                      check(TokenKind::EndOfFile) || atLineBreak();
    if (bare) {
        auto undef = std::make_unique<UndefinedLit>();
        undef->span = kw.span;
        node->argument = std::move(undef);
        return node;
    }
    node->argument = parseAssign();
    if (!node->argument) return nullptr;
    node->span.end = node->argument->span.end;
    return node;
}

// `[ Symbol.iterator ]`, the one computed member name a CLASS BODY reads.
//
// The shape is matched syntactically — on the two identifiers — because that is
// how much of the computed-key grammar bronze admits in a class body, and an
// unrecognised one is refused by name rather than approximated. What it hands
// back is the EXPRESSION, not a name: `Symbol.iterator` is an ordinary member
// read (20.4.2.5) whose value is the well-known symbol, so it is evaluated at
// class-definition time like the object-literal form already was. That is what
// makes a program which rebinds `Symbol` get its own answer here rather than
// bronze's, and it is why the well-known symbols needed no representation in the
// IL: a symbol key reaches the runtime through the ordinary computed-key path.
ExprPtr Parser::matchSymbolIteratorKey() {
    if (!check(TokenKind::LBracket)) return nullptr;
    if (!(peek(1).kind == TokenKind::Identifier && peek(1).text == "Symbol")) return nullptr;
    if (peek(2).kind != TokenKind::Dot) return nullptr;
    if (!(peek(3).kind == TokenKind::Identifier && peek(3).text == "iterator")) return nullptr;
    if (peek(4).kind != TokenKind::RBracket) return nullptr;
    const Span span = peek().span;
    advance();  // '['
    advance();  // 'Symbol'
    advance();  // '.'
    advance();  // 'iterator'
    advance();  // ']'
    auto symbol = std::make_unique<Ident>();
    symbol->span = span;
    symbol->name = "Symbol";
    auto member = std::make_unique<MemberAccess>();
    member->span = span;
    member->object = std::move(symbol);
    member->property = "iterator";
    return member;
}

bool Parser::parseGeneratorTail(ast::FunctionExpr& fn) {
    fn.isGenerator = true;
    if (!expect(TokenKind::LParen, "'(' after a generator name")) return false;
    if (!parseParams(fn.params)) return false;
    if (!expect(TokenKind::RParen, "')' after generator parameters")) return false;
    if (match(TokenKind::Colon)) fn.returnType = parseTypeAnnotation();

    // A parameter list is NOT part of the generator body: 15.5.1 forbids
    // `yield` in one, and the flag is raised only over the braces.
    const bool savedInBody = inGeneratorBody_;
    inGeneratorBody_ = true;
    fn.body = parseFunctionBody(fn.strict);
    inGeneratorBody_ = savedInBody;
    if (diags_.hasErrors()) return false;

    // Dots, for the reason an object-literal method's IL symbol has them: a
    // source identifier cannot contain one, so no temporary this rewrite
    // declares can be confused with — or shadowed by — a binding the program
    // wrote. The file qualifier keeps two files' first generators apart.
    const size_t ordinal = generatorOrdinal_++;
    const std::string prefix =
        "gen." +
        (fileId_ == 0 ? std::to_string(ordinal)
                      : std::to_string(fileId_) + "." + std::to_string(ordinal)) +
        ".";
    if (!liftYields(fn.body, prefix, diags_)) return false;

    fn.span.end = previous().span.end;
    // After the body, because a generator's own `"use strict"` is what makes
    // its parameter list subject to the rule — the same ordering every other
    // function form takes.
    return checkStrictParams(fn.params, fn.strict);
}

}  // namespace bronze
