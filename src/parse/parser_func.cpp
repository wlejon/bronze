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

    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        // `export default function () {}` (ECMA-262 16.2.3.7): a hoisted
        // declaration with no name. It is still hoisted and still a
        // declaration, so it gets the reserved word `default` as its name —
        // which nothing in the source can spell, so it cannot collide.
        fn->name = defaultName;
    } else {
        const Token* name = expect(TokenKind::Identifier, "function name");
        if (!name) return nullptr;
        fn->name = std::string(name->text);
    }

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseBlock();
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

    if (check(TokenKind::LBrace)) {
        fn->body = parseBlock();
    } else {
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
// and can have neither a default nor a pattern (docs/0017).
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
                                                               std::string& outName) {
    const bool isGetter = kind == ast::AccessorKind::Getter;
    const char* word = isGetter ? "getter" : "setter";

    if (check(TokenKind::LBracket)) {
        error((std::string("unsupported construct: a computed ") + word +
               " name (`get [e]() {}`)")
                  .c_str());
        return nullptr;
    }
    Span nameSpan = peek().span;
    if (check(TokenKind::StringLiteral)) {
        const Token& sTok = advance();
        outName = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
    } else if (isIdentifierName(peek().kind)) {
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
    fn->name = std::string(isGetter ? "get " : "set ") + outName;
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

    fn->body = parseBlock();
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
// name the parser is inside (docs/0012 decision 5), so an object literal
// method nested in a class method must not inherit that binding: `super.m()`
// there would silently call the enclosing class's parent. The two fields are
// therefore saved and cleared for the body, which turns that into the
// existing "super outside a class method" error.
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
    fn->body = parseBlock();
    inClassMethod_ = savedInClassMethod;
    currentClassSuper_ = savedClassSuper;

    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `class Name [extends Base] { members }`. A class introduces no runtime
// concept - it is the constructor function plus its prototype, and lowering
// desugars it into exactly that (docs/0012 decision 5). What the parser
// owes is the shape: which member is the constructor, which are static, and
// which class each `super` in a body belongs to.
//
// Everything ES2015+ puts in a class body that bronze has not built -
// fields, getters and setters, computed keys, generators - is diagnosed by
// name here rather than mis-parsed as a method.
ast::StmtPtr Parser::parseClass(const std::string& defaultName) {
    const Token& kw = advance();  // 'class'
    auto cls = std::make_unique<ClassDecl>();
    cls->span.begin = kw.span.begin;
    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        cls->name = defaultName;  // `export default class {}`, as above
    } else {
        const Token* nameTok = expect(TokenKind::Identifier, "class name");
        if (!nameTok) return nullptr;
        cls->name = std::string(nameTok->text);
    }

    if (match(TokenKind::KwExtends)) {
        const Token* base = expect(TokenKind::Identifier, "base class name after 'extends'");
        if (!base) return nullptr;
        cls->superName = std::string(base->text);
    }
    if (!expect(TokenKind::LBrace, "'{' to open a class body")) return nullptr;

    // Every `super` inside a method belongs to THIS class, and the parser is
    // the only place that knows which class that is.
    const std::string savedSuper = currentClassSuper_;
    const bool savedInMethod = inClassMethod_;
    currentClassSuper_ = cls->superName;
    inClassMethod_ = true;

    bool ok = true;
    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated class body");
            ok = false;
            break;
        }
        if (match(TokenKind::Semicolon)) continue;  // a stray `;` between members is legal

        ClassMethod member;
        // `static` is not a reserved word: it names a member when something
        // follows it, and is an ordinary method name in `static() {}`.
        if (check(TokenKind::Identifier) && peek().text == "static" &&
            peek(1).kind != TokenKind::LParen) {
            advance();
            member.isStatic = true;
        }
        if (check(TokenKind::Star)) {
            error("unsupported construct: generator method in a class body");
            ok = false;
            break;
        }
        if (check(TokenKind::LBracket)) {
            error("unsupported construct: computed method name in a class body");
            ok = false;
            break;
        }
        // `async` is contextual as well, and a ClassElementName on the SAME
        // line is what makes it a modifier (ECMA-262 15.8.1 forbids a line
        // terminator after it) — `async() {}` is a method named `async` and
        // `async = 1` a field named `async`. Named here because the field
        // diagnostic below fires on the identifier-then-identifier shape and
        // would call an async method a field, which it never is.
        if (check(TokenKind::Identifier) && peek().text == "async" && !peek(1).newlineBefore &&
            (isIdentifierName(peek(1).kind) || peek(1).kind == TokenKind::Star ||
             peek(1).kind == TokenKind::LBracket ||
             peek(1).kind == TokenKind::StringLiteral ||
             peek(1).kind == TokenKind::NumberLiteral)) {
            error("unsupported construct: async method in a class body");
            ok = false;
            break;
        }
        // `get`/`set` are contextual here too: `get() {}` is a method named
        // `get`, and only a following name makes this an accessor.
        if (check(TokenKind::Identifier) && (peek().text == "get" || peek().text == "set") &&
            (peek(1).kind == TokenKind::Identifier || peek(1).kind == TokenKind::StringLiteral ||
             peek(1).kind == TokenKind::LBracket)) {
            const ast::AccessorKind kind =
                peek().text == "get" ? ast::AccessorKind::Getter : ast::AccessorKind::Setter;
            advance();  // 'get' / 'set'
            auto accessorFn = parseAccessorMember(kind, member.name);
            if (!accessorFn) {
                ok = false;
                break;
            }
            accessorFn->name = cls->name + "." + accessorFn->name;
            member.accessor = kind;
            member.fn = std::move(accessorFn);
            cls->methods.push_back(std::move(member));
            continue;
        }
        // A ClassElementName is a PropertyName is an IdentifierName (15.7), so
        // `delete()` and `return()` are ordinary method names on a class.
        const Token* memberName = expectPropertyName("class member name");
        if (!memberName) {
            ok = false;
            break;
        }
        member.name = std::string(memberName->text);
        if (!check(TokenKind::LParen)) {
            // `x = 1;` or `x;` - a field, which runs in the constructor and
            // is not built yet. Named rather than read as a broken method.
            error("unsupported construct: class field (only methods are supported)");
            ok = false;
            break;
        }
        member.isConstructor = !member.isStatic && member.name == "constructor";

        auto fn = std::make_unique<FunctionExpr>();
        fn->span.begin = memberName->span.begin;
        fn->name = cls->name + "." + member.name;
        advance();  // '('
        if (!parseParams(fn->params)) return nullptr;
        if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
        if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
        fn->body = parseBlock();
        if (diags_.hasErrors()) return nullptr;
        fn->span.end = peek().span.begin;
        member.fn = std::move(fn);
        cls->methods.push_back(std::move(member));
    }

    currentClassSuper_ = savedSuper;
    inClassMethod_ = savedInMethod;
    if (!ok) return nullptr;
    if (!expect(TokenKind::RBrace, "'}' to close a class body")) return nullptr;

    // Lowering wants exactly one constructor, always, so a class that writes
    // none gets the one ECMA-262 15.7.14 says it has — synthesized here, in
    // the parser, so lowering never has two shapes to reason about.
    //
    // For a base class that is an empty body. For a DERIVED one it is
    // `constructor(...args) { super(...args); }`, which is exactly a rest
    // parameter and a spread and nothing else: the forwarding has to be
    // arity-preserving, and before those existed it was a named error rather
    // than a quietly truncated argument list (docs/0012 decision 5).
    bool hasCtor = false;
    for (const auto& m : cls->methods) hasCtor = hasCtor || m.isConstructor;
    if (!hasCtor) {
        ClassMethod ctor;
        ctor.name = "constructor";
        ctor.isConstructor = true;
        ctor.fn = std::make_unique<FunctionExpr>();
        ctor.fn->name = cls->name + ".constructor";
        ctor.fn->span = cls->span;
        if (!cls->superName.empty()) {
            Param rest;
            rest.name = "args";
            rest.isRest = true;
            rest.span = cls->span;
            ctor.fn->params.push_back(std::move(rest));

            auto argsRef = std::make_unique<Ident>();
            argsRef->span = cls->span;
            argsRef->name = "args";
            auto spread = std::make_unique<SpreadElement>();
            spread->span = cls->span;
            spread->argument = std::move(argsRef);

            auto call = std::make_unique<SuperCall>();
            call->span = cls->span;
            call->baseName = cls->superName;
            call->args.push_back(std::move(spread));

            auto stmt = std::make_unique<ExprStmt>();
            stmt->span = cls->span;
            stmt->expr = std::move(call);
            ctor.fn->body.push_back(std::move(stmt));
        }
        cls->methods.insert(cls->methods.begin(), std::move(ctor));
    }
    cls->span.end = peek().span.begin;
    return cls;
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

    if (check(TokenKind::Identifier)) {
        fn->name = std::string(advance().text);
    }

    if (!expect(TokenKind::LParen, "'(' after function")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseBlock();
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

}  // namespace bronze
