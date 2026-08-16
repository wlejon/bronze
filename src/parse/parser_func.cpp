// Every form that has a parameter list and a body: function declarations and
// expressions, arrows, and classes — which are the same thing again, with the
// bookkeeping that tells each `super` which class it belongs to.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

StmtPtr Parser::parseFunctionDecl(bool isExported, const std::string& defaultName) {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionDecl>();
    fn->span.begin = kw.span.begin;
    fn->isExported = isExported;
    const bool isGenerator = match(TokenKind::Star);

    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        // `export default function () {}` (ECMA-262 16.2.3.7): a hoisted
        // declaration with no name. It is still hoisted and still a
        // declaration, so it gets the reserved word `default` as its name —
        // which nothing in the source can spell, so it cannot collide.
        fn->name = defaultName;
    } else {
        const Token* name = expect(TokenKind::Identifier, "function name");
        if (!name) return nullptr;
        if (!checkStrictBindingName(name->text, name->span, "function name")) return nullptr;
        fn->name = std::string(name->text);
    }

    // `function* g() {}` takes the same route a generator METHOD does: a
    // FunctionExpr is filled in and its pieces moved across, because a
    // declaration and an expression differ in where the value lands and in
    // nothing about the body.
    if (isGenerator) {
        GeneratorScopeGuard guard(*this);
        ast::FunctionExpr shell;
        shell.span = fn->span;
        shell.name = fn->name;
        if (!parseGeneratorTail(shell)) return nullptr;
        fn->params = std::move(shell.params);
        fn->returnType = std::move(shell.returnType);
        fn->body = std::move(shell.body);
        fn->strict = shell.strict;
        fn->isGenerator = true;
        fn->span.end = peek().span.begin;
        return fn;
    }

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    {
        GeneratorScopeGuard guard(*this);
        fn->body = parseFunctionBody(fn->strict);
    }
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}
// `x => ...` and `(a, b) => ...`. Deciding between an arrow's parameter
// list and a parenthesized expression needs unbounded lookahead in general,
// so this scans forward for the `)` that matches the `(` and asks what
// follows it. Cheaper than parse-and-backtrack, and it cannot half-consume
// the input on a wrong guess.
bool Parser::looksLikeArrow() const {
    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Arrow) return true;
    if (!check(TokenKind::LParen)) return false;
    size_t depth = 0;
    for (size_t i = 0;; ++i) {
        const TokenKind kind = peek(i).kind;
        if (kind == TokenKind::EndOfFile) return false;
        if (kind == TokenKind::LParen) ++depth;
        else if (kind == TokenKind::RParen) {
            if (--depth == 0) {
                // A return-type annotation sits between the `)` and the
                // `=>`; the annotation itself is one token today.
                if (peek(i + 1).kind == TokenKind::Arrow) return true;
                return peek(i + 1).kind == TokenKind::Colon &&
                       peek(i + 3).kind == TokenKind::Arrow;
            }
        }
    }
}

ExprPtr Parser::parseArrowFunction() {
    auto fn = std::make_unique<FunctionExpr>();
    fn->isArrow = true;
    fn->span.begin = peek().span.begin;

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

    // An arrow written inside a generator body is not itself a generator, so
    // `yield` is an ordinary identifier in it and `return` an ordinary return.
    GeneratorScopeGuard guard(*this);
    if (check(TokenKind::LBrace)) {
        fn->body = parseFunctionBody(fn->strict);
    } else {
        // A concise body has no Directive Prologue to read — there is no
        // StatementList for one to be the head of — so an arrow written this
        // way is strict exactly when the code around it is.
        fn->strict = strict_;
        // An expression body IS a return, and is stored as one so that
        // every consumer below — capture analysis, inference, lowering —
        // sees one shape of function body and not two.
        auto value = parseAssign();
        if (!value) return nullptr;
        auto ret = std::make_unique<ReturnStmt>();
        ret->span = value->span;
        ret->value = std::move(value);
        fn->body.push_back(std::move(ret));
    }
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}
// One parameter list, for every function form there is — declaration,
// expression, arrow and class method. One copy, because four copies of this
// loop are four places for the rules below to drift apart.
//
// A parameter is a binding target (a name or a pattern), optionally with a
// default; or a rest parameter, which takes what is left and so must be last
// and can have neither a default nor a pattern.
bool Parser::parseParams(std::vector<ast::Param>& out) {
    while (!check(TokenKind::RParen)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated parameter list");
            return false;
        }
        Param p;
        p.span.begin = peek().span.begin;
        const bool isRest = match(TokenKind::Ellipsis);
        p.isRest = isRest;

        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
            if (isRest) {
                error("a rest parameter must be a plain name");
                return false;
            }
            p.pattern = parsePattern();
            if (!p.pattern) return false;
        } else {
            const Token* param = expect(TokenKind::Identifier, "parameter name");
            if (!param) return false;
            // What fires here is the ENCLOSING code's mode. A list belonging to
            // a function whose own directive has not been read yet is checked
            // again by `checkStrictParams`, once the body has said so.
            if (!checkStrictBindingName(param->text, param->span, "parameter")) return false;
            p.name = std::string(param->text);
        }

        if (match(TokenKind::Colon)) p.typeAnnotation = parseTypeAnnotation();
        if (check(TokenKind::Assign)) {
            if (isRest) {
                error("a rest parameter may not have a default value");
                return false;
            }
            advance();
            p.defaultValue = parseAssign();
            if (!p.defaultValue) return false;
        }
        p.span.end = peek().span.begin;
        out.push_back(std::move(p));

        if (isRest) {
            if (!check(TokenKind::RParen)) {
                error("a rest parameter must be the last parameter");
                return false;
            }
            break;
        }
        if (!match(TokenKind::Comma)) break;
    }
    return true;
}

// `get k() {}` / `set k(v) {}`, with the contextual `get` or `set` already
// consumed. Object literals and class bodies share this because they share
// the whole of the syntax: what differs is the enumerable attribute the
// runtime gives the result, which is the caller's to decide.
//
// The arity rules are 15.4.1's and are early errors, not runtime ones: a
// getter that took a parameter could never be given one, and a setter that
// took none would silently discard every write.
std::unique_ptr<ast::FunctionExpr> Parser::parseAccessorMember(ast::AccessorKind kind,
                                                               std::string& outName,
                                                               ast::ExprPtr* outKeyExpr) {
    const bool isGetter = kind == ast::AccessorKind::Getter;
    const char* word = isGetter ? "getter" : "setter";

    Span nameSpan = peek().span;
    if (check(TokenKind::LBracket)) {
        if (!outKeyExpr) {
            error((std::string("unsupported construct: a computed ") + word +
                   " name (`get [e]() {}`)")
                      .c_str());
            return nullptr;
        }
        advance();  // '['
        *outKeyExpr = parseAssign();
        if (!*outKeyExpr || !expect(TokenKind::RBracket, "']' after computed accessor name")) {
            return nullptr;
        }
        outName = isGetter ? "get computed" : "set computed";
    } else if (check(TokenKind::StringLiteral)) {
        const Token& sTok = advance();
        outName = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
    } else if (isIdentifierName(peek().kind) || check(TokenKind::PrivateName)) {
        // A PrivateName reaches here only from a class body: an object
        // literal's `get`/`set` lookahead does not admit one, so `{ get #x() {}
        // }` never becomes an accessor and is refused where it is written.
        outName = std::string(advance().text);
    } else {
        // A numeric accessor name lands here with the object literal's own
        // rule: the name is ToString(Number), which is the runtime's
        // formatter and not something the parser may reimplement.
        error((std::string("expected a ") + word +
               " name: an identifier or a string literal")
                  .c_str());
        return nullptr;
    }

    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = nameSpan.begin;
    fn->name = (outKeyExpr && *outKeyExpr)
                   ? outName
                   : (std::string(isGetter ? "get " : "set ") + outName);
    if (!expect(TokenKind::LParen, "'(' after an accessor name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after accessor parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    const size_t want = isGetter ? 0u : 1u;
    if (fn->params.size() != want) {
        error((std::string("a ") + word + " must take exactly " +
               (isGetter ? "no parameters" : "one parameter"))
                  .c_str());
        return nullptr;
    }
    if (!fn->params.empty() && fn->params[0].isRest) {
        error("a setter's parameter may not be a rest parameter");
        return nullptr;
    }

    {
        GeneratorScopeGuard guard(*this);
        fn->body = parseFunctionBody(fn->strict);
    }
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `m(params) { body }` with the name already consumed — the tail of every
// MethodDefinition (ECMA-262 15.4), which an object literal and a class body
// spell identically.
//
// A method's `super` is resolved against its HOME OBJECT, and an object
// literal's home object is the literal. bronze resolves `super` by the class
// name the parser is inside, so an object literal method nested in a class
// method must not inherit that binding: `super.m()` there would silently call
// the enclosing class's parent. The two fields are therefore saved and cleared
// for the body, which turns that into the existing "super outside a class
// method" error.
std::unique_ptr<ast::FunctionExpr> Parser::parseMethodTail(const std::string& name,
                                                           Span nameSpan) {
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = nameSpan.begin;
    fn->name = name;
    if (!expect(TokenKind::LParen, "'(' after a method name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after method parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    const bool savedInClassMethod = inClassMethod_;
    const std::string savedClassSuper = currentClassSuper_;
    inClassMethod_ = false;
    currentClassSuper_.clear();
    GeneratorScopeGuard guard(*this);
    fn->body = parseFunctionBody(fn->strict);
    inClassMethod_ = savedInClassMethod;
    currentClassSuper_ = savedClassSuper;

    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `super(...)` and `super.m` - only inside a class method, and only in a
// class that has a parent, which is where the name they resolve against
// comes from.
ExprPtr Parser::parseSuper() {
    const Token& kw = advance();  // 'super'
    if (!inClassMethod_) {
        error("unsupported construct: super outside a class method");
        return nullptr;
    }
    if (currentClassSuper_.empty()) {
        error("super in a class with no 'extends'");
        return nullptr;
    }
    if (check(TokenKind::LParen)) {
        advance();
        auto call = std::make_unique<SuperCall>();
        call->span.begin = kw.span.begin;
        call->baseName = currentClassSuper_;
        if (!parseArgumentList(call->args)) return nullptr;
        call->span.end = peek().span.begin;
        return call;
    }
    if (match(TokenKind::Dot)) {
        const Token* member = expectPropertyName("property name after 'super.'");
        if (!member) return nullptr;
        auto mem = std::make_unique<SuperMember>();
        mem->span = {kw.span.begin, member->span.end};
        mem->baseName = currentClassSuper_;
        mem->property = std::string(member->text);
        return mem;
    }
    error("super must be called or have a property read from it");
    return nullptr;
}

ExprPtr Parser::parseFunctionExpr() {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = kw.span.begin;
    const bool isGenerator = match(TokenKind::Star);

    if (check(TokenKind::Identifier)) {
        const Token& nameTok = advance();
        if (!checkStrictBindingName(nameTok.text, nameTok.span, "function name")) return nullptr;
        fn->name = std::string(nameTok.text);
    }

    GeneratorScopeGuard guard(*this);
    if (isGenerator) {
        if (!parseGeneratorTail(*fn)) return nullptr;
        return fn;
    }

    if (!expect(TokenKind::LParen, "'(' after function")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseFunctionBody(fn->strict);
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

}  // namespace bronze
